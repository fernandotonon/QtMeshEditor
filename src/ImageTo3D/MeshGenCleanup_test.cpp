#include "MeshGenCleanup.h"
#include "MeshGenPredictor.h"

#include <gtest/gtest.h>


namespace {

// n×n grid of unit quads on the XZ plane, each split into two triangles.
void makeGrid(int n, std::vector<float>& pos, std::vector<uint32_t>& idx)
{
    pos.clear();
    idx.clear();
    for (int z = 0; z <= n; ++z)
        for (int x = 0; x <= n; ++x) {
            pos.push_back(float(x));
            pos.push_back(0.0f);
            pos.push_back(float(z));
        }
    auto v = [n](int x, int z) { return uint32_t(z * (n + 1) + x); };
    for (int z = 0; z < n; ++z)
        for (int x = 0; x < n; ++x) {
            idx.insert(idx.end(), {v(x, z), v(x, z + 1), v(x + 1, z)});
            idx.insert(idx.end(), {v(x + 1, z), v(x, z + 1), v(x + 1, z + 1)});
        }
}

} // namespace

TEST(MeshGenCleanup, KeepsACleanMeshUnchanged)
{
    std::vector<float> pos;
    std::vector<uint32_t> idx;
    makeGrid(3, pos, idx);
    const auto pos0 = pos;
    const auto idx0 = idx;
    const auto rep = MeshGenCleanup::removeDegenerates(pos, idx);
    EXPECT_EQ(rep.total(), 0);
    EXPECT_EQ(rep.removedVertices, 0);
    EXPECT_EQ(pos, pos0);
    EXPECT_EQ(idx, idx0);
}

TEST(MeshGenCleanup, DropsEveryDegenerateKind)
{
    std::vector<float> pos;
    std::vector<uint32_t> idx;
    makeGrid(1, pos, idx);                  // 4 verts, 2 good triangles
    // vertex 4: on the edge 0-1 (collinear); vertex 5: coincides with 0.
    pos.insert(pos.end(), {0.5f, 0.0f, 0.0f});
    pos.insert(pos.end(), {0.0f, 0.0f, 0.0f});
    idx.insert(idx.end(), {0, 0, 3});       // repeated index
    idx.insert(idx.end(), {0, 4, 1});       // collinear needle
    idx.insert(idx.end(), {0, 5, 2});       // coincident corners
    idx.insert(idx.end(), {1, 2, 0});       // duplicate of tri 0 (reversed)
    std::vector<float> uv(pos.size() / 3 * 2);
    for (size_t i = 0; i < uv.size(); ++i) uv[i] = float(i);

    const auto rep = MeshGenCleanup::removeDegenerates(pos, idx, {{&uv, 2}});
    EXPECT_EQ(rep.repeatedIndex, 1);
    EXPECT_EQ(rep.zeroArea, 2);
    EXPECT_EQ(rep.duplicate, 1);
    ASSERT_EQ(idx.size(), 6u);
    // The two orphaned extra vertices are compacted out of every stream.
    EXPECT_EQ(rep.removedVertices, 2);
    EXPECT_EQ(pos.size(), 12u);
    ASSERT_EQ(uv.size(), 8u);
    // Vertex order is first-reference order, and the UV stream follows it.
    for (uint32_t i : idx) {
        ASSERT_LT(i, 4u);
    }
}

TEST(MeshGenCleanup, ThinButRealTriangleSurvives)
{
    // Height 1e-3 over a unit base is a sliver, not a degenerate — the
    // threshold is relative to the triangle's own longest edge (1e-5).
    std::vector<float> pos = {0, 0, 0, 1, 0, 0, 0.5f, 1e-3f, 0};
    std::vector<uint32_t> idx = {0, 1, 2};
    const auto rep = MeshGenCleanup::removeDegenerates(pos, idx);
    EXPECT_EQ(rep.total(), 0);
    EXPECT_EQ(idx.size(), 3u);
}

TEST(MeshGenCleanup, ThresholdIsScaleInvariant)
{
    // The same collinear needle at 1000x scale is still removed, and a
    // proportionally-thin real triangle at 1e-3 scale is still kept.
    std::vector<float> big = {0, 0, 0, 1000, 0, 0, 500, 1e-6f, 0};
    std::vector<uint32_t> i1 = {0, 1, 2};
    EXPECT_EQ(MeshGenCleanup::removeDegenerates(big, i1).zeroArea, 1);
    std::vector<float> tiny = {0, 0, 0, 1e-3f, 0, 0, 5e-4f, 1e-6f, 0};
    std::vector<uint32_t> i2 = {0, 1, 2};
    EXPECT_EQ(MeshGenCleanup::removeDegenerates(tiny, i2).zeroArea, 0);
}

TEST(MeshGenCleanup, FinalizeTopologyRemovesDegeneratesByDefault)
{
    MeshGenPredictor::Result r;
    r.ok = true;
    makeGrid(2, r.positions, r.indices);     // 8 tris
    r.indices.insert(r.indices.end(), {0, 0, 1});   // one degenerate
    r.vertexCount = int(r.positions.size() / 3);
    r.triangleCount = int(r.indices.size() / 3);

    MeshGenPredictor::Options opts;
    EXPECT_TRUE(opts.removeDegenerates);    // default on
    MeshGenPredictor::finalizeTopology(r, opts);
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(r.degeneratesRemoved, 1);
    EXPECT_EQ(r.triangleCount, 8);
    EXPECT_EQ(r.indices.size(), 24u);
}

TEST(MeshGenCleanup, FinalizeTopologyRespectsOptOut)
{
    MeshGenPredictor::Result r;
    r.ok = true;
    makeGrid(1, r.positions, r.indices);
    r.indices.insert(r.indices.end(), {0, 0, 1});
    r.triangleCount = 3;
    MeshGenPredictor::Options opts;
    opts.removeDegenerates = false;
    MeshGenPredictor::finalizeTopology(r, opts);
    EXPECT_EQ(r.indices.size(), 9u);
}
