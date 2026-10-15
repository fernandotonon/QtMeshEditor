#include "MeshGenCleanup.h"
#include "MeshGenPredictor.h"
#include "QuadRemesh/QuadriFlowRunner.h"

#include <gtest/gtest.h>

#include <set>

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

TEST(MeshGenCleanup, GridPairsIntoQuads)
{
    std::vector<float> pos;
    std::vector<uint32_t> idx;
    makeGrid(4, pos, idx);                   // 32 tris = 16 squares
    std::vector<std::vector<uint32_t>> faces;
    const auto rep = MeshGenCleanup::pairIntoQuads(pos, idx, faces);
    ASSERT_TRUE(rep.ok);
    EXPECT_EQ(rep.quads, 16);
    EXPECT_EQ(rep.triangles, 0);
    EXPECT_DOUBLE_EQ(rep.quadDominance(), 1.0);
    // Every quad covers exactly the four corners of one grid square.
    std::set<std::set<uint32_t>> cells;
    for (const auto& f : faces) {
        ASSERT_EQ(f.size(), 4u);
        cells.insert(std::set<uint32_t>(f.begin(), f.end()));
    }
    EXPECT_EQ(cells.size(), 16u);
}

TEST(MeshGenCleanup, FinalizeTopologyCleansAndQuadifiesResult)
{
    MeshGenPredictor::Result r;
    r.ok = true;
    makeGrid(2, r.positions, r.indices);     // 8 tris
    r.indices.insert(r.indices.end(), {0, 0, 1});   // one degenerate
    r.vertexCount = int(r.positions.size() / 3);
    r.triangleCount = int(r.indices.size() / 3);

    MeshGenPredictor::Options opts;
    EXPECT_TRUE(opts.removeDegenerates);    // default on
    EXPECT_FALSE(opts.quadify);             // default off
    opts.quadify = true;
    MeshGenPredictor::finalizeTopology(r, opts);
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(r.degeneratesRemoved, 1);
    EXPECT_EQ(r.triangleCount, 8);
    EXPECT_EQ(r.quadCount, 4);
    EXPECT_EQ(r.faces.size(), 4u);
    // The render buffer still holds the original triangles.
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
    EXPECT_TRUE(r.faces.empty());
}

TEST(MeshGenCleanup, FinalizeTopologyQuadRemeshRebakesTexture)
{
    if (QuadriFlowRunner::defaultHelperPath().isEmpty())
        GTEST_SKIP() << "qtmesh_quadriflow helper not built";
    // A closed box (welded) with UVs + a texture: the remesh must replace
    // the topology with quads and hand back a re-baked texture + normal map.
    MeshGenPredictor::Result r;
    r.ok = true;
    const float c[8][3] = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0},
                           {0, 0, 1}, {1, 0, 1}, {1, 1, 1}, {0, 1, 1}};
    const int q[6][4] = {{0, 3, 2, 1}, {4, 5, 6, 7}, {0, 1, 5, 4},
                         {2, 3, 7, 6}, {1, 2, 6, 5}, {0, 4, 7, 3}};
    // Subdivide each face 8x8 so QuadriFlow has something to work with.
    const int n = 8;
    for (const auto& f : q) {
        const uint32_t base = uint32_t(r.positions.size() / 3);
        for (int y = 0; y <= n; ++y)
            for (int x = 0; x <= n; ++x) {
                const float u = float(x) / n, v = float(y) / n;
                for (int k = 0; k < 3; ++k) {
                    const float a = c[f[0]][k] + (c[f[1]][k] - c[f[0]][k]) * u;
                    const float b = c[f[3]][k] + (c[f[2]][k] - c[f[3]][k]) * u;
                    r.positions.push_back(a + (b - a) * v);
                }
                r.uvs.insert(r.uvs.end(), {u, v});
            }
        for (int y = 0; y < n; ++y)
            for (int x = 0; x < n; ++x) {
                const uint32_t i0 = base + y * (n + 1) + x, i1 = i0 + 1, i2 = i0 + n + 2, i3 = i0 + n + 1;
                r.indices.insert(r.indices.end(), {i0, i1, i2, i0, i2, i3});
            }
    }
    r.texture = QImage(64, 64, QImage::Format_RGB888);
    r.texture.fill(QColor(200, 40, 40));
    r.vertexCount = int(r.positions.size() / 3);
    r.triangleCount = int(r.indices.size() / 3);

    MeshGenPredictor::Options opts;
    opts.quadRemesh = true;
    opts.quadRemeshTarget = 300;
    MeshGenPredictor::finalizeTopology(r, opts);
    ASSERT_TRUE(r.ok) << r.error.toStdString();
    EXPECT_GT(r.quadCount, 150);
    EXPECT_FALSE(r.faces.empty());
    ASSERT_EQ(r.uvs.size(), r.positions.size() / 3 * 2);
    EXPECT_FALSE(r.normalMap.isNull());
    ASSERT_FALSE(r.texture.isNull());
    // The flat red survives the re-bake.
    const QRgb mid = r.texture.pixel(r.texture.width() / 2, r.texture.height() / 2);
    EXPECT_GT(qRed(mid), 150);
    EXPECT_LT(qGreen(mid), 90);
}
