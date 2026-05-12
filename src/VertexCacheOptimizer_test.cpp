#include <gtest/gtest.h>

#include "VertexCacheOptimizer.h"

#include <QJsonArray>
#include <QJsonObject>

#include <algorithm>
#include <random>
#include <vector>

// Pure-data tests — no Ogre required, so they run on every CI build.

namespace {

// Synthesize a triangle strip whose triangles share two vertices with their
// neighbour (the case where post-T&L cache helps most). Returns an index
// buffer with 3 * (stripLen - 2) entries.
std::vector<uint32_t> makeStrip(uint32_t stripLen)
{
    std::vector<uint32_t> idx;
    if (stripLen < 3) return idx;
    idx.reserve(3 * (stripLen - 2));
    for (uint32_t i = 0; i + 2 < stripLen; ++i) {
        idx.push_back(i);
        idx.push_back(i + 1);
        idx.push_back(i + 2);
    }
    return idx;
}

// Shuffled version of a strip — destroys the cache-friendly order. Used to
// confirm forsyth() recovers ACMR back toward the strip's natural value.
std::vector<uint32_t> shuffleTriangles(std::vector<uint32_t> idx, unsigned seed = 12345)
{
    if (idx.empty() || idx.size() % 3 != 0) return idx;
    const size_t triCount = idx.size() / 3;
    std::vector<size_t> order(triCount);
    for (size_t i = 0; i < triCount; ++i) order[i] = i;
    std::mt19937 rng(seed);
    std::shuffle(order.begin(), order.end(), rng);

    std::vector<uint32_t> out;
    out.reserve(idx.size());
    for (size_t t : order) {
        out.push_back(idx[t * 3]);
        out.push_back(idx[t * 3 + 1]);
        out.push_back(idx[t * 3 + 2]);
    }
    return out;
}

} // namespace

// ---- Primitive ACMR sanity ------------------------------------------------

TEST(VertexCacheOptimizerTest, AcmrEmpty)
{
    std::vector<uint32_t> idx;
    EXPECT_DOUBLE_EQ(0.0, VertexCacheOptimizer::computeAcmr(idx));
}

TEST(VertexCacheOptimizerTest, AcmrSingleTriangle)
{
    std::vector<uint32_t> idx = {0, 1, 2};
    // 3 unique verts, cold cache → 3 misses, 1 triangle → ACMR 3.0.
    EXPECT_DOUBLE_EQ(3.0, VertexCacheOptimizer::computeAcmr(idx));
}

TEST(VertexCacheOptimizerTest, AcmrPerfectStrip)
{
    // A long strip has near-optimal locality: each new tri introduces
    // exactly 1 fresh vertex (the other 2 are cached from the prior tri).
    // For N=100 the ACMR should be ~ (100 + 2) / (100 - 2 + 1) ≈ 1.04 -> very low.
    auto idx = makeStrip(100);
    const double acmr = VertexCacheOptimizer::computeAcmr(idx);
    EXPECT_LT(acmr, 1.2) << "Strip ACMR should be near-optimal, got " << acmr;
}

TEST(VertexCacheOptimizerTest, AcmrShuffledStripIsWorse)
{
    auto strip = makeStrip(100);
    const double good = VertexCacheOptimizer::computeAcmr(strip);
    const double bad  = VertexCacheOptimizer::computeAcmr(shuffleTriangles(strip));
    EXPECT_GT(bad, good)
        << "Shuffling should increase ACMR (good=" << good << " bad=" << bad << ")";
}

// ---- Forsyth correctness --------------------------------------------------

TEST(VertexCacheOptimizerTest, ForsythEmptyInputs)
{
    std::vector<uint32_t> idx;
    EXPECT_FALSE(VertexCacheOptimizer::forsyth(idx, 0));

    std::vector<uint32_t> partial = {0, 1};
    EXPECT_FALSE(VertexCacheOptimizer::forsyth(partial, 3));
}

TEST(VertexCacheOptimizerTest, ForsythSingleTrianglePassthrough)
{
    std::vector<uint32_t> idx = {0, 1, 2};
    std::vector<uint32_t> copy = idx;
    EXPECT_TRUE(VertexCacheOptimizer::forsyth(idx, 3));
    // Single tri has nothing to reorder; only the set of indices is invariant.
    std::sort(idx.begin(), idx.end());
    std::sort(copy.begin(), copy.end());
    EXPECT_EQ(idx, copy);
}

TEST(VertexCacheOptimizerTest, ForsythReducesAcmrOnShuffledMesh)
{
    // 50-triangle strip, shuffled, then optimized — ACMR must drop
    // significantly. Use a relatively long strip so the cache-friendly
    // ordering has room to work.
    auto strip = makeStrip(50);
    auto shuffled = shuffleTriangles(strip);
    const double before = VertexCacheOptimizer::computeAcmr(shuffled);

    ASSERT_TRUE(VertexCacheOptimizer::forsyth(shuffled, 50));
    const double after = VertexCacheOptimizer::computeAcmr(shuffled);

    EXPECT_LT(after, before)
        << "Forsyth should reduce ACMR (before=" << before << " after=" << after << ")";
    // Generous threshold so we don't flake on platform-specific FP details.
    EXPECT_LT(after, before * 0.85)
        << "Expected at least 15% improvement, got "
        << ((before - after) / before * 100.0) << "%";
}

TEST(VertexCacheOptimizerTest, ForsythPreservesTriangleSet)
{
    // The optimiser must permute triangles, never invent or drop them.
    auto strip = makeStrip(30);
    auto shuffled = shuffleTriangles(strip);

    // Pull the original triangle set (as sorted vertex triples) so we can
    // compare orderless.
    auto canonicalTriSet = [](std::vector<uint32_t> idx) {
        std::vector<std::array<uint32_t, 3>> tris;
        for (size_t t = 0; t + 2 < idx.size(); t += 3) {
            std::array<uint32_t, 3> tri = {idx[t], idx[t + 1], idx[t + 2]};
            std::sort(tri.begin(), tri.end());
            tris.push_back(tri);
        }
        std::sort(tris.begin(), tris.end());
        return tris;
    };
    const auto before = canonicalTriSet(shuffled);

    ASSERT_TRUE(VertexCacheOptimizer::forsyth(shuffled, 30));
    const auto after = canonicalTriSet(shuffled);

    EXPECT_EQ(before, after) << "Forsyth must not introduce or drop triangles";
}

TEST(VertexCacheOptimizerTest, ForsythRejectsOutOfRangeIndex)
{
    // Index 99 doesn't exist when vertexCount is 3 — must fail cleanly.
    std::vector<uint32_t> bad = {0, 1, 99};
    EXPECT_FALSE(VertexCacheOptimizer::forsyth(bad, 3));
}

// ---- Serialisation --------------------------------------------------------

TEST(VertexCacheOptimizerTest, JsonShape)
{
    VertexCacheReport report;
    SubMeshCacheReport sr;
    sr.meshName = "M.mesh";
    sr.submeshIndex = 0;
    sr.triangleCount = 100;
    sr.acmrBefore = 2.5;
    sr.acmrAfter = 1.0;
    sr.reordered = true;
    report.submeshes.append(sr);
    report.totalTriangles = 100;
    report.weightedAcmrBefore = 2.5;
    report.weightedAcmrAfter = 1.0;
    report.totalReordered = 1;

    const QJsonObject obj = VertexCacheOptimizer::toJson(report);
    ASSERT_TRUE(obj.contains("submeshes"));
    ASSERT_TRUE(obj.contains("totals"));
    EXPECT_EQ(1, obj["submeshes"].toArray().size());
    EXPECT_EQ(2.5, obj["totals"].toObject()["acmrBefore"].toDouble());
    EXPECT_EQ(1.0, obj["totals"].toObject()["acmrAfter"].toDouble());
    EXPECT_NEAR(60.0, obj["totals"].toObject()["improvementPercent"].toDouble(), 0.001);
}

TEST(VertexCacheOptimizerTest, JsonEmptyReport)
{
    VertexCacheReport empty;
    const QJsonObject obj = VertexCacheOptimizer::toJson(empty);
    EXPECT_TRUE(obj.contains("submeshes"));
    EXPECT_TRUE(obj.contains("totals"));
    EXPECT_EQ(0, obj["submeshes"].toArray().size());
    EXPECT_EQ(0, obj["totals"].toObject()["totalTriangles"].toInt());
}

TEST(VertexCacheOptimizerTest, TextHasHeader)
{
    VertexCacheReport report;
    SubMeshCacheReport sr;
    sr.meshName = "M.mesh";
    sr.submeshIndex = 0;
    sr.triangleCount = 50;
    sr.acmrBefore = 2.0;
    sr.acmrAfter = 0.9;
    sr.reordered = true;
    report.submeshes.append(sr);
    report.totalTriangles = 50;
    report.weightedAcmrBefore = 2.0;
    report.weightedAcmrAfter = 0.9;
    report.totalReordered = 1;

    const QString text = VertexCacheOptimizer::toText(report);
    EXPECT_TRUE(text.contains("Vertex Cache Analysis"));
    EXPECT_TRUE(text.contains("M.mesh"));
    EXPECT_TRUE(text.contains("(reordered)"));
    EXPECT_TRUE(text.contains("improvement"));
}

TEST(VertexCacheOptimizerTest, TextHandlesEmpty)
{
    VertexCacheReport empty;
    const QString text = VertexCacheOptimizer::toText(empty);
    EXPECT_TRUE(text.contains("Vertex Cache Analysis"));
    EXPECT_TRUE(text.contains("(no submeshes)"));
}

TEST(VertexCacheOptimizerTest, ImprovementPercentDivByZero)
{
    VertexCacheReport report;
    // weightedAcmrBefore = 0 → improvement() must return 0 without dividing.
    EXPECT_DOUBLE_EQ(0.0, report.improvement());
}
