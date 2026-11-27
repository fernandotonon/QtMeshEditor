// Pure-data tests for the mesh-buffer quality core (#678).

#include "MeshQuality.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <random>
#include <set>
#include <tuple>

using namespace MeshQuality;

namespace {

// n x n quad grid in the XY plane, triangles in a shuffled order so the
// vertex cache and the vertex fetch order are both poor.
Geometry shuffledGrid(int n, unsigned seed = 7)
{
    Geometry g;
    for (int y = 0; y <= n; ++y)
        for (int x = 0; x <= n; ++x)
            g.positions.insert(g.positions.end(), {float(x), float(y), 0.0f});
    g.vertexCount = size_t((n + 1) * (n + 1));
    std::vector<std::array<uint32_t, 3>> tris;
    for (int y = 0; y < n; ++y) {
        for (int x = 0; x < n; ++x) {
            const uint32_t a = uint32_t(y * (n + 1) + x), b = a + 1, c = a + uint32_t(n + 1), d = c + 1;
            tris.push_back({a, b, d});
            tris.push_back({a, d, c});
        }
    }
    std::mt19937 rng(seed);
    std::shuffle(tris.begin(), tris.end(), rng);
    for (const auto& t : tris)
        g.indices.insert(g.indices.end(), t.begin(), t.end());
    return g;
}

using Corner = std::tuple<float, float, float>;
using Tri = std::tuple<Corner, Corner, Corner>;

// The triangles as position triples, rotation-canonical (winding kept).
std::multiset<Tri> triangleSet(const std::vector<float>& pos, const std::vector<uint32_t>& idx)
{
    std::multiset<Tri> out;
    auto c = [&pos](uint32_t i) { return Corner{pos[i * 3], pos[i * 3 + 1], pos[i * 3 + 2]}; };
    for (size_t t = 0; t + 2 < idx.size(); t += 3) {
        Corner k[3] = {c(idx[t]), c(idx[t + 1]), c(idx[t + 2])};
        const int r = int(std::min_element(k, k + 3) - k);
        out.insert({k[r], k[(r + 1) % 3], k[(r + 2) % 3]});
    }
    return out;
}

// New positions laid out by a FixResult remap.
std::vector<float> remappedPositions(const Geometry& g, const FixResult& r)
{
    if (r.remap.empty())
        return g.positions;
    const auto bytes = remapStream(reinterpret_cast<const unsigned char*>(g.positions.data()),
                                   g.vertexCount, sizeof(float) * 3, r.remap, r.newVertexCount);
    std::vector<float> out(r.newVertexCount * 3);
    std::memcpy(out.data(), bytes.data(), bytes.size());
    return out;
}

} // namespace

TEST(MeshQuality, ShuffledGridHasAvoidableCacheAndFetchCost)
{
    const Geometry g = shuffledGrid(40);
    const Metrics m = analyze(g);
    EXPECT_EQ(m.triangles, 40u * 40u * 2u);
    EXPECT_EQ(m.vertices, 41u * 41u);
    EXPECT_EQ(m.degenerateTriangles, 0u);
    EXPECT_EQ(m.redundantVertices, 0u);
    EXPECT_GT(m.acmr, m.acmrOptimized);
    EXPECT_GT(m.acmrGainPct(), 30.0);
    EXPECT_GT(m.overfetch, 1.0);
    EXPECT_GE(m.overdraw, 1.0);   // flat grid: one layer, nothing to gain
    EXPECT_NEAR(m.overdrawOptimized, m.overdraw, 1e-6);
}

TEST(MeshQuality, OptimizedOrderReportsNoGain)
{
    Geometry g = shuffledGrid(30);
    FixOptions o;
    o.vertexCache = true;
    o.vertexFetch = true;
    const FixResult r = fix(g, o);
    Geometry fixedGeom;
    fixedGeom.positions = remappedPositions(g, r);
    fixedGeom.indices = r.indices;
    fixedGeom.vertexCount = r.newVertexCount;
    const Metrics m = analyze(fixedGeom);
    EXPECT_LT(m.acmrGainPct(), 1.0);
    EXPECT_LT(m.overfetchGainPct(), 1.0);
}

TEST(MeshQuality, DegenerateIsScaleInvariant)
{
    for (float s : {1e-6f, 1.0f, 1e6f}) {
        Geometry g;
        g.positions = {0, 0, 0, s, 0, 0, 0, s, 0,          // fine
                       0, 0, 0, s, 0, 0, 2 * s, 1e-7f * s, 0};   // sliver: height 1e-7 of its length
        g.vertexCount = 6;
        EXPECT_FALSE(isDegenerate(g, 0, 1, 2)) << "scale " << s;
        EXPECT_TRUE(isDegenerate(g, 3, 4, 5)) << "scale " << s;
        EXPECT_TRUE(isDegenerate(g, 0, 0, 1)) << "repeated corner, scale " << s;
    }
}

TEST(MeshQuality, RedundantVerticesUseEveryAttribute)
{
    // Two triangles meeting on an edge whose two corners are duplicated.
    Geometry g;
    g.positions = {0, 0, 0, 1, 0, 0, 0, 1, 0,
                   1, 0, 0, 0, 1, 0, 1, 1, 0};
    g.vertexCount = 6;
    g.indices = {0, 1, 2, 3, 5, 4};
    EXPECT_EQ(analyze(g).redundantVertices, 2u);

    // Same positions, different UV on vertex 3: only vertex 4 is redundant.
    const float uv[6][2] = {{0, 0}, {1, 0}, {0, 1}, {0.5f, 0}, {0, 1}, {1, 1}};
    g.keyStride = sizeof(float) * 5;
    g.keys.resize(6 * g.keyStride);
    for (int v = 0; v < 6; ++v) {
        std::memcpy(g.keys.data() + v * g.keyStride, &g.positions[size_t(v) * 3], sizeof(float) * 3);
        std::memcpy(g.keys.data() + v * g.keyStride + 12, uv[v], sizeof(float) * 2);
    }
    EXPECT_EQ(analyze(g).redundantVertices, 1u);
}

TEST(MeshQuality, WeldKeepsTheTriangles)
{
    Geometry g;
    g.positions = {0, 0, 0, 1, 0, 0, 0, 1, 0,
                   1, 0, 0, 0, 1, 0, 1, 1, 0,
                   9, 9, 9};                   // unreferenced
    g.vertexCount = 7;
    g.indices = {0, 1, 2, 3, 5, 4};
    FixOptions o;
    o.weld = true;
    const FixResult r = fix(g, o);
    EXPECT_TRUE(r.changed);
    EXPECT_EQ(r.newVertexCount, 4u);
    EXPECT_EQ(r.verticesWelded, 2u);
    EXPECT_EQ(r.verticesDropped, 1u);
    EXPECT_EQ(r.remap[6], ~0u);
    EXPECT_EQ(triangleSet(remappedPositions(g, r), r.indices), triangleSet(g.positions, g.indices));
}

TEST(MeshQuality, FixPipelineKeepsTheTriangleSet)
{
    const Geometry g = shuffledGrid(25);
    FixOptions o;
    o.removeDegenerates = o.weld = o.vertexCache = o.overdraw = o.vertexFetch = true;
    const FixResult r = fix(g, o);
    EXPECT_TRUE(r.changed);
    EXPECT_EQ(r.trianglesAfter, r.trianglesBefore);
    EXPECT_EQ(triangleSet(remappedPositions(g, r), r.indices), triangleSet(g.positions, g.indices));
}

TEST(MeshQuality, RemovesOnlyDegenerateTriangles)
{
    Geometry g = shuffledGrid(4);
    const size_t good = g.indices.size() / 3;
    g.indices.insert(g.indices.end(), {0, 0, 1});   // repeated corner
    g.indices.insert(g.indices.end(), {0, 1, 2});   // collinear (row y = 0)
    EXPECT_EQ(analyze(g).degenerateTriangles, 2u);
    FixOptions o;
    o.removeDegenerates = true;
    const FixResult r = fix(g, o);
    EXPECT_EQ(r.degeneratesRemoved, 2u);
    EXPECT_EQ(r.trianglesAfter, good);
    EXPECT_TRUE(r.remap.empty());   // index-only fix
}

TEST(MeshQuality, SimplifyReachesTheTarget)
{
    // A bumpy grid simplifies; a flat one would collapse to two triangles.
    Geometry g = shuffledGrid(40);
    for (size_t v = 0; v < g.vertexCount; ++v)
        g.positions[v * 3 + 2] = std::sin(g.positions[v * 3] * 0.4f) * std::cos(g.positions[v * 3 + 1] * 0.3f);
    FixOptions o;
    o.simplifyTargetTriangles = 800;
    o.simplifyMaxError = 1.0f;
    const FixResult r = fix(g, o);
    EXPECT_TRUE(r.simplified);
    EXPECT_LE(r.trianglesAfter, 800u);
    EXPECT_GT(r.trianglesAfter, 0u);
}

TEST(MeshQuality, NoOptionsOrBadInputChangesNothing)
{
    const Geometry g = shuffledGrid(5);
    EXPECT_FALSE(fix(g, FixOptions{}).changed);

    Geometry bad = g;
    bad.indices.push_back(9999);
    bad.indices.push_back(0);
    bad.indices.push_back(1);
    FixOptions o;
    o.vertexCache = true;
    const FixResult r = fix(bad, o);
    EXPECT_FALSE(r.changed);
    EXPECT_EQ(r.indices, bad.indices);
    EXPECT_EQ(analyze(bad).acmr, 0.0);   // not measured, no crash
}

TEST(MeshQuality, GainPctOnlyCountsImprovements)
{
    EXPECT_DOUBLE_EQ(Metrics::gainPct(2.0, 1.0), 50.0);
    EXPECT_DOUBLE_EQ(Metrics::gainPct(1.0, 1.2), 0.0);
    EXPECT_DOUBLE_EQ(Metrics::gainPct(-1.0, -1.0), 0.0);   // overdraw not measured
}

TEST(MeshQuality, RemapStreamFirstWriterWins)
{
    const unsigned char data[4] = {10, 20, 30, 40};
    const std::vector<uint32_t> remap = {1, 0, ~0u, 0};
    const auto out = remapStream(data, 4, 1, remap, 2);
    ASSERT_EQ(out.size(), 2u);
    EXPECT_EQ(out[0], 20);
    EXPECT_EQ(out[1], 10);
}
