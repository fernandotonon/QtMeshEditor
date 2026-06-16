#include <gtest/gtest.h>

#include <QJsonArray>
#include <QJsonObject>
#include <QString>

#include "ExportOptimizer.h"

#include <cstdint>
#include <vector>

// Pure-data coverage suite for ExportOptimizer. None of these tests
// touch Ogre or require a display — they exercise the serializers
// (toJson/toText), the report helpers (improvementPct/empty), the
// OptimizeFlags free functions, and the pure-data computeAcmr helper
// (which only needs meshoptimizer, already linked into the test
// binary). Distinct file + suite name from ExportOptimizer_test.cpp
// to avoid ODR / duplicate-registration clashes.

namespace {

// Build a per-submesh report row by hand so we can assemble whole
// ExportOptimizeReport structs without going through Ogre.
ExportOptimizeSubMeshReport makeSubReport(const QString& name, int index,
                                          int tris, int verts,
                                          double before, double after,
                                          bool cache, bool overdraw,
                                          bool fetch)
{
    ExportOptimizeSubMeshReport sr;
    sr.meshName       = name;
    sr.submeshIndex   = index;
    sr.triangleCount  = tris;
    sr.vertexCount    = verts;
    sr.acmrBefore     = before;
    sr.acmrAfter      = after;
    sr.vertexCacheRun = cache;
    sr.overdrawRun    = overdraw;
    sr.vertexFetchRun = fetch;
    return sr;
}

} // namespace

// ---------------------------------------------------------------------------
// OptimizeFlags free functions: operator| / operator& / any()
// ---------------------------------------------------------------------------

TEST(ExportOptimizerCoverageTest, FlagsOrCombinesBits) {
    OptimizeFlags combined = OptimizeFlags::VertexCache | OptimizeFlags::Overdraw;
    EXPECT_EQ(static_cast<uint32_t>(combined),
              static_cast<uint32_t>(OptimizeFlags::VertexCache) |
                  static_cast<uint32_t>(OptimizeFlags::Overdraw));
}

TEST(ExportOptimizerCoverageTest, FlagsAllContainsEveryBit) {
    EXPECT_TRUE(any(OptimizeFlags::All & OptimizeFlags::VertexCache));
    EXPECT_TRUE(any(OptimizeFlags::All & OptimizeFlags::Overdraw));
    EXPECT_TRUE(any(OptimizeFlags::All & OptimizeFlags::VertexFetch));
}

TEST(ExportOptimizerCoverageTest, FlagsAndMasksOutAbsentBit) {
    OptimizeFlags only = OptimizeFlags::VertexCache;
    // VertexCache & Overdraw share no bits -> None.
    EXPECT_FALSE(any(only & OptimizeFlags::Overdraw));
    // VertexCache & VertexCache keeps the bit.
    EXPECT_TRUE(any(only & OptimizeFlags::VertexCache));
}

TEST(ExportOptimizerCoverageTest, AnyOnNoneIsFalse) {
    EXPECT_FALSE(any(OptimizeFlags::None));
}

TEST(ExportOptimizerCoverageTest, AnyOnSingleBitIsTrue) {
    EXPECT_TRUE(any(OptimizeFlags::VertexFetch));
}

TEST(ExportOptimizerCoverageTest, FlagsOrIsAssociativeToAll) {
    OptimizeFlags all = OptimizeFlags::VertexCache | OptimizeFlags::Overdraw |
                        OptimizeFlags::VertexFetch;
    EXPECT_EQ(static_cast<uint32_t>(all),
              static_cast<uint32_t>(OptimizeFlags::All));
}

// ---------------------------------------------------------------------------
// ExportOptimizeReport::empty()
// ---------------------------------------------------------------------------

TEST(ExportOptimizerCoverageTest, EmptyReportIsEmpty) {
    ExportOptimizeReport report;
    EXPECT_TRUE(report.empty());
}

TEST(ExportOptimizerCoverageTest, PopulatedReportIsNotEmpty) {
    ExportOptimizeReport report;
    report.submeshes.append(makeSubReport("m", 0, 10, 6, 1.0, 0.5,
                                          true, true, false));
    EXPECT_FALSE(report.empty());
}

// ---------------------------------------------------------------------------
// ExportOptimizeReport::improvementPct() — both branches
// ---------------------------------------------------------------------------

TEST(ExportOptimizerCoverageTest, ImprovementPctZeroBeforeIsGuardedToZero) {
    ExportOptimizeReport report;
    report.weightedAcmrBefore = 0.0;   // div-by-zero guard branch
    report.weightedAcmrAfter  = 0.0;
    EXPECT_DOUBLE_EQ(report.improvementPct(), 0.0);
}

TEST(ExportOptimizerCoverageTest, ImprovementPctZeroBeforeNonzeroAfterStillZero) {
    ExportOptimizeReport report;
    report.weightedAcmrBefore = 0.0;   // still hits the guard
    report.weightedAcmrAfter  = 1.5;
    EXPECT_DOUBLE_EQ(report.improvementPct(), 0.0);
}

TEST(ExportOptimizerCoverageTest, ImprovementPctComputesRatio) {
    ExportOptimizeReport report;
    report.weightedAcmrBefore = 2.0;
    report.weightedAcmrAfter  = 1.0;
    // (2 - 1) / 2 * 100 = 50%
    EXPECT_DOUBLE_EQ(report.improvementPct(), 50.0);
}

TEST(ExportOptimizerCoverageTest, ImprovementPctNegativeWhenRegressed) {
    ExportOptimizeReport report;
    report.weightedAcmrBefore = 1.0;
    report.weightedAcmrAfter  = 1.5;
    // (1 - 1.5) / 1 * 100 = -50%
    EXPECT_DOUBLE_EQ(report.improvementPct(), -50.0);
}

TEST(ExportOptimizerCoverageTest, ImprovementPctZeroWhenNoChange) {
    ExportOptimizeReport report;
    report.weightedAcmrBefore = 1.2;
    report.weightedAcmrAfter  = 1.2;
    EXPECT_DOUBLE_EQ(report.improvementPct(), 0.0);
}

// ---------------------------------------------------------------------------
// ExportOptimizer::computeAcmr — edge cases + non-trivial path
// ---------------------------------------------------------------------------

TEST(ExportOptimizerCoverageTest, ComputeAcmrEmptyIndicesReturnsZero) {
    std::vector<uint32_t> empty;
    EXPECT_DOUBLE_EQ(ExportOptimizer::computeAcmr(empty, 4), 0.0);
}

TEST(ExportOptimizerCoverageTest, ComputeAcmrZeroVertexCountReturnsZero) {
    std::vector<uint32_t> indices = {0, 1, 2};
    EXPECT_DOUBLE_EQ(ExportOptimizer::computeAcmr(indices, 0), 0.0);
}

TEST(ExportOptimizerCoverageTest, ComputeAcmrEmptyAndZeroBothReturnZero) {
    std::vector<uint32_t> empty;
    EXPECT_DOUBLE_EQ(ExportOptimizer::computeAcmr(empty, 0), 0.0);
}

TEST(ExportOptimizerCoverageTest, ComputeAcmrSingleTriangleIsPositive) {
    // One triangle, cold cache: 3 vertices fetched / 1 triangle = ACMR 3.0
    std::vector<uint32_t> indices = {0, 1, 2};
    const double acmr = ExportOptimizer::computeAcmr(indices, 3);
    EXPECT_NEAR(acmr, 3.0, 1e-6);
}

TEST(ExportOptimizerCoverageTest, ComputeAcmrTwoTriangleQuadIsReasonable) {
    // Two triangles sharing an edge over 4 vertices: {0,1,2, 0,2,3}.
    // Cold cache fetches 0,1,2 then re-fetches 0,2 (depending on cache),
    // so ACMR sits between 1.0 and 3.0. Just assert it's in a sane range.
    std::vector<uint32_t> indices = {0, 1, 2, 0, 2, 3};
    const double acmr = ExportOptimizer::computeAcmr(indices, 4);
    EXPECT_GT(acmr, 0.0);
    EXPECT_LE(acmr, 3.0);
}

TEST(ExportOptimizerCoverageTest, ComputeAcmrLargeCacheLowersAcmr) {
    // A long fan that reuses vertex 0 repeatedly: a 32-entry cache (the
    // configured size) easily holds the shared vertex, so ACMR should be
    // well below the cold-cache worst case of 3.0.
    std::vector<uint32_t> indices;
    for (uint32_t i = 1; i + 1 < 20; ++i) {
        indices.push_back(0);
        indices.push_back(i);
        indices.push_back(i + 1);
    }
    const double acmr = ExportOptimizer::computeAcmr(indices, 20);
    EXPECT_GT(acmr, 0.0);
    EXPECT_LT(acmr, 3.0);
}

// ---------------------------------------------------------------------------
// ExportOptimizer::toText — empty branch + populated summary
// ---------------------------------------------------------------------------

TEST(ExportOptimizerCoverageTest, ToTextEmptyReportShowsNoSubmeshesBranch) {
    ExportOptimizeReport report;
    const QString text = ExportOptimizer::toText(report);
    EXPECT_TRUE(text.contains("Export Optimization"));
    EXPECT_TRUE(text.contains("(no submeshes to optimize)"));
    // The populated summary must NOT appear.
    EXPECT_FALSE(text.contains("improvement"));
}

TEST(ExportOptimizerCoverageTest, ToTextPopulatedReportShowsSummary) {
    ExportOptimizeReport report;
    report.submeshes.append(makeSubReport("Mesh", 0, 100, 60, 2.0, 1.0,
                                          true, true, false));
    report.submeshes.append(makeSubReport("Mesh", 1, 50, 30, 2.0, 1.0,
                                          true, false, false));
    report.submeshesOptimized = 2;
    report.totalTriangles     = 150;
    report.weightedAcmrBefore = 2.0;
    report.weightedAcmrAfter  = 1.0;

    const QString text = ExportOptimizer::toText(report);
    EXPECT_TRUE(text.contains("Export Optimization"));
    EXPECT_FALSE(text.contains("(no submeshes to optimize)"));
    EXPECT_TRUE(text.contains("Optimized 2 of 2 submesh(es)"));
    // ACMR before/after formatted to 3 decimals.
    EXPECT_TRUE(text.contains("2.000"));
    EXPECT_TRUE(text.contains("1.000"));
    // improvementPct = 50% to 1 decimal.
    EXPECT_TRUE(text.contains("50.0"));
    EXPECT_TRUE(text.contains("improvement"));
}

TEST(ExportOptimizerCoverageTest, ToTextSubmeshCountMatchesListSize) {
    ExportOptimizeReport report;
    // 3 submeshes present but only 1 actually ran an optimizer.
    report.submeshes.append(makeSubReport("M", 0, 10, 6, 1.5, 1.0,
                                          true, false, false));
    report.submeshes.append(makeSubReport("M", 1, 10, 6, 1.5, 1.5,
                                          false, false, false));
    report.submeshes.append(makeSubReport("M", 2, 10, 6, 1.5, 1.5,
                                          false, false, false));
    report.submeshesOptimized = 1;
    report.totalTriangles     = 30;
    report.weightedAcmrBefore = 1.5;
    report.weightedAcmrAfter  = 1.333;

    const QString text = ExportOptimizer::toText(report);
    EXPECT_TRUE(text.contains("Optimized 1 of 3 submesh(es)"));
}

// ---------------------------------------------------------------------------
// ExportOptimizer::toJson — all keys + submeshes array contents
// ---------------------------------------------------------------------------

TEST(ExportOptimizerCoverageTest, ToJsonEmptyReportHasAllTopLevelKeys) {
    ExportOptimizeReport report;
    const QJsonObject obj = ExportOptimizer::toJson(report);

    ASSERT_TRUE(obj.contains("submeshes"));
    EXPECT_TRUE(obj["submeshes"].isArray());
    EXPECT_TRUE(obj["submeshes"].toArray().isEmpty());

    EXPECT_TRUE(obj.contains("weightedAcmrBefore"));
    EXPECT_TRUE(obj.contains("weightedAcmrAfter"));
    EXPECT_TRUE(obj.contains("totalTriangles"));
    EXPECT_TRUE(obj.contains("submeshesOptimized"));
    EXPECT_TRUE(obj.contains("improvementPct"));

    EXPECT_DOUBLE_EQ(obj["weightedAcmrBefore"].toDouble(), 0.0);
    EXPECT_DOUBLE_EQ(obj["weightedAcmrAfter"].toDouble(), 0.0);
    EXPECT_EQ(obj["totalTriangles"].toInt(), 0);
    EXPECT_EQ(obj["submeshesOptimized"].toInt(), 0);
    EXPECT_DOUBLE_EQ(obj["improvementPct"].toDouble(), 0.0);
}

TEST(ExportOptimizerCoverageTest, ToJsonPopulatedTopLevelValues) {
    ExportOptimizeReport report;
    report.submeshes.append(makeSubReport("CharMesh", 0, 100, 60, 2.0, 1.0,
                                          true, true, false));
    report.submeshesOptimized = 1;
    report.totalTriangles     = 100;
    report.weightedAcmrBefore = 2.0;
    report.weightedAcmrAfter  = 1.0;

    const QJsonObject obj = ExportOptimizer::toJson(report);
    EXPECT_DOUBLE_EQ(obj["weightedAcmrBefore"].toDouble(), 2.0);
    EXPECT_DOUBLE_EQ(obj["weightedAcmrAfter"].toDouble(), 1.0);
    EXPECT_EQ(obj["totalTriangles"].toInt(), 100);
    EXPECT_EQ(obj["submeshesOptimized"].toInt(), 1);
    EXPECT_DOUBLE_EQ(obj["improvementPct"].toDouble(), 50.0);
}

TEST(ExportOptimizerCoverageTest, ToJsonSubmeshArrayEntryHasEveryField) {
    ExportOptimizeReport report;
    report.submeshes.append(makeSubReport("BodyMesh", 3, 42, 21, 2.5, 1.25,
                                          true, false, true));
    const QJsonObject obj = ExportOptimizer::toJson(report);

    const QJsonArray arr = obj["submeshes"].toArray();
    ASSERT_EQ(arr.size(), 1);
    const QJsonObject so = arr[0].toObject();

    EXPECT_EQ(so["mesh"].toString(), QString("BodyMesh"));
    EXPECT_EQ(so["submeshIndex"].toInt(), 3);
    EXPECT_EQ(so["triangleCount"].toInt(), 42);
    EXPECT_EQ(so["vertexCount"].toInt(), 21);
    EXPECT_DOUBLE_EQ(so["acmrBefore"].toDouble(), 2.5);
    EXPECT_DOUBLE_EQ(so["acmrAfter"].toDouble(), 1.25);
    EXPECT_TRUE(so["vertexCacheRun"].toBool());
    EXPECT_FALSE(so["overdrawRun"].toBool());
    EXPECT_TRUE(so["vertexFetchRun"].toBool());
}

TEST(ExportOptimizerCoverageTest, ToJsonSubmeshArrayPreservesOrderAndCount) {
    ExportOptimizeReport report;
    report.submeshes.append(makeSubReport("A", 0, 10, 6, 1.0, 0.5,
                                          true, false, false));
    report.submeshes.append(makeSubReport("B", 1, 20, 12, 2.0, 1.0,
                                          true, true, false));
    report.submeshes.append(makeSubReport("C", 2, 30, 18, 3.0, 1.5,
                                          false, false, false));

    const QJsonObject obj = ExportOptimizer::toJson(report);
    const QJsonArray arr = obj["submeshes"].toArray();
    ASSERT_EQ(arr.size(), 3);
    EXPECT_EQ(arr[0].toObject()["mesh"].toString(), QString("A"));
    EXPECT_EQ(arr[1].toObject()["mesh"].toString(), QString("B"));
    EXPECT_EQ(arr[2].toObject()["mesh"].toString(), QString("C"));
    EXPECT_EQ(arr[0].toObject()["submeshIndex"].toInt(), 0);
    EXPECT_EQ(arr[2].toObject()["submeshIndex"].toInt(), 2);
}
