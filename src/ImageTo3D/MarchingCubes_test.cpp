#include "MarchingCubes.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <unordered_map>
#include <vector>

// Unit tests for the native Lorensen marching cubes (epic #764, slice A #765).
// Pure-data — no Ogre, no GL — so these run unconditionally on Linux CI (the
// SDF-proof requirement in issue #765: extract a closed mesh from a synthetic
// sphere/cube density field).

namespace {

// Sample a scalar field on an n^3 grid over [lo,hi]^3 from an inside-positive
// implicit function f(x,y,z) (>0 inside the surface, =0 on it).
template <typename F>
std::vector<float> sampleGrid(int n, float lo, float hi, F&& f)
{
    std::vector<float> g(static_cast<size_t>(n) * n * n);
    const float step = (hi - lo) / float(n - 1);
    for (int z = 0; z < n; ++z)
        for (int y = 0; y < n; ++y)
            for (int x = 0; x < n; ++x) {
                const float wx = lo + x * step;
                const float wy = lo + y * step;
                const float wz = lo + z * step;
                g[static_cast<size_t>(z) * n * n + static_cast<size_t>(y) * n + x] =
                    f(wx, wy, wz);
            }
    return g;
}

// Count how many triangles reference each undirected edge. A watertight
// (closed, 2-manifold) surface has EVERY edge shared by exactly 2 triangles.
std::map<std::pair<uint32_t, uint32_t>, int>
edgeUseCounts(const MarchingCubes::Mesh& m)
{
    std::map<std::pair<uint32_t, uint32_t>, int> counts;
    auto bump = [&](uint32_t a, uint32_t b) {
        if (a > b) std::swap(a, b);
        counts[{a, b}]++;
    };
    for (size_t t = 0; t + 2 < m.indices.size(); t += 3) {
        bump(m.indices[t + 0], m.indices[t + 1]);
        bump(m.indices[t + 1], m.indices[t + 2]);
        bump(m.indices[t + 2], m.indices[t + 0]);
    }
    return counts;
}

} // namespace

TEST(MarchingCubesTest, EmptyFieldYieldsEmptyMesh)
{
    // Field entirely below the iso level → no surface, no crash.
    const int n = 16;
    auto g = sampleGrid(n, -1.f, 1.f, [](float, float, float) { return -5.f; });
    auto m = MarchingCubes::extract(g.data(), n, n, n, 0.0f, {-1, -1, -1}, {1, 1, 1});
    EXPECT_EQ(m.vertexCount, 0);
    EXPECT_EQ(m.triangleCount, 0);
    EXPECT_TRUE(m.positions.empty());
    EXPECT_TRUE(m.indices.empty());
}

TEST(MarchingCubesTest, NullFieldAndDegenerateGridAreSafe)
{
    std::array<float, 3> lo{-1, -1, -1}, hi{1, 1, 1};
    auto m0 = MarchingCubes::extract(nullptr, 8, 8, 8, 0.0f, lo, hi);
    EXPECT_EQ(m0.vertexCount, 0);

    std::vector<float> tiny(1, 1.0f);
    auto m1 = MarchingCubes::extract(tiny.data(), 1, 1, 1, 0.0f, lo, hi);
    EXPECT_EQ(m1.vertexCount, 0);   // n<2 on every axis → nothing to march

    // Short-field contract: a declared buffer length below nx*ny*nz must yield
    // an empty mesh instead of reading past the buffer.
    std::vector<float> shortField(10, 1.0f);
    auto m2 = MarchingCubes::extract(shortField.data(), 8, 8, 8, 0.0f, lo, hi,
                                     shortField.size());
    EXPECT_EQ(m2.vertexCount, 0);
}

TEST(MarchingCubesTest, SphereIsClosedAndOnSurface)
{
    // Inside-positive sphere SDF: f = R - |p|, surface at iso 0 (radius R).
    const int   n = 48;
    const float R = 0.6f;
    auto g = sampleGrid(n, -1.f, 1.f, [R](float x, float y, float z) {
        return R - std::sqrt(x * x + y * y + z * z);
    });
    auto m = MarchingCubes::extract(g.data(), n, n, n, 0.0f, {-1, -1, -1}, {1, 1, 1});

    ASSERT_GT(m.vertexCount, 100);
    ASSERT_GT(m.triangleCount, 100);
    EXPECT_EQ(static_cast<int>(m.positions.size()), m.vertexCount * 3);
    EXPECT_EQ(static_cast<int>(m.indices.size()), m.triangleCount * 3);

    // Watertight: every edge used by exactly two triangles (closed 2-manifold).
    const auto counts = edgeUseCounts(m);
    int boundaryEdges = 0, nonManifold = 0;
    for (const auto& kv : counts) {
        if (kv.second == 1) ++boundaryEdges;
        else if (kv.second > 2) ++nonManifold;
    }
    EXPECT_EQ(boundaryEdges, 0) << "sphere surface should have no open edges";
    EXPECT_EQ(nonManifold, 0) << "sphere surface should be 2-manifold";

    // Winding/orientation: for an inside-positive field, each triangle's face
    // normal (CCW winding, right-hand rule) must point OUTWARD — i.e. away from
    // the sphere centre (the origin). This guards the winding fix that stopped
    // generated meshes rendering inside-out (normals appeared inverted).
    {
        int outward = 0, total = 0;
        for (size_t t = 0; t + 2 < m.indices.size(); t += 3) {
            const uint32_t a = m.indices[t], b = m.indices[t + 1], c = m.indices[t + 2];
            const float* pa = &m.positions[a * 3];
            const float* pb = &m.positions[b * 3];
            const float* pc = &m.positions[c * 3];
            const float e1[3] = {pb[0]-pa[0], pb[1]-pa[1], pb[2]-pa[2]};
            const float e2[3] = {pc[0]-pa[0], pc[1]-pa[1], pc[2]-pa[2]};
            const float fn[3] = {e1[1]*e2[2]-e1[2]*e2[1], e1[2]*e2[0]-e1[0]*e2[2], e1[0]*e2[1]-e1[1]*e2[0]};
            // Triangle centroid ~= outward direction from origin for a sphere.
            const float cx = (pa[0]+pb[0]+pc[0])/3.f, cy = (pa[1]+pb[1]+pc[1])/3.f, cz = (pa[2]+pb[2]+pc[2])/3.f;
            if (fn[0]*cx + fn[1]*cy + fn[2]*cz > 0.f) ++outward;
            ++total;
        }
        // The overwhelming majority must face outward (a few near-tangent tris
        // can be ambiguous at the discretisation limit).
        EXPECT_GT(outward, total * 0.95) << "sphere triangles must wind outward";
    }

    // Every vertex sits ~on the sphere of radius R (within one cell of slop).
    const float cell = 2.0f / float(n - 1);
    for (int i = 0; i < m.vertexCount; ++i) {
        const float x = m.positions[3 * i + 0];
        const float y = m.positions[3 * i + 1];
        const float z = m.positions[3 * i + 2];
        const float r = std::sqrt(x * x + y * y + z * z);
        EXPECT_NEAR(r, R, cell) << "vertex " << i << " off the sphere";
    }
}

TEST(MarchingCubesTest, BoxMatchesAABB)
{
    // Inside-positive box SDF (half-extent H): f = H - max(|x|,|y|,|z|).
    const int   n = 40;
    const float H = 0.5f;
    auto g = sampleGrid(n, -1.f, 1.f, [H](float x, float y, float z) {
        return H - std::max(std::fabs(x), std::max(std::fabs(y), std::fabs(z)));
    });
    auto m = MarchingCubes::extract(g.data(), n, n, n, 0.0f, {-1, -1, -1}, {1, 1, 1});
    ASSERT_GT(m.vertexCount, 24);

    // The extracted AABB should hug ±H within a cell.
    float mn[3] = {1e9f, 1e9f, 1e9f}, mx[3] = {-1e9f, -1e9f, -1e9f};
    for (int i = 0; i < m.vertexCount; ++i)
        for (int a = 0; a < 3; ++a) {
            const float v = m.positions[3 * i + a];
            mn[a] = std::min(mn[a], v);
            mx[a] = std::max(mx[a], v);
        }
    const float cell = 2.0f / float(n - 1);
    for (int a = 0; a < 3; ++a) {
        EXPECT_NEAR(mn[a], -H, cell);
        EXPECT_NEAR(mx[a], H, cell);
    }
}

TEST(MarchingCubesTest, ThresholdShiftShrinksSurface)
{
    // TripoSR feeds density with an iso `threshold`; raising the iso on the same
    // field should carve a smaller surface. Prove the isoLevel parameter works.
    const int n = 40;
    auto g = sampleGrid(n, -1.f, 1.f, [](float x, float y, float z) {
        return 1.0f - std::sqrt(x * x + y * y + z * z);   // radius grows as iso ↓
    });
    auto big   = MarchingCubes::extract(g.data(), n, n, n, 0.2f, {-1, -1, -1}, {1, 1, 1});
    auto small = MarchingCubes::extract(g.data(), n, n, n, 0.6f, {-1, -1, -1}, {1, 1, 1});
    ASSERT_GT(big.vertexCount, 0);
    ASSERT_GT(small.vertexCount, 0);

    auto maxRadius = [](const MarchingCubes::Mesh& m) {
        float r = 0.f;
        for (int i = 0; i < m.vertexCount; ++i) {
            const float x = m.positions[3 * i + 0], y = m.positions[3 * i + 1], z = m.positions[3 * i + 2];
            r = std::max(r, std::sqrt(x * x + y * y + z * z));
        }
        return r;
    };
    EXPECT_GT(maxRadius(big), maxRadius(small));
}

TEST(MarchingCubesTest, VerticesAreWeldedNotDuplicatedPerTriangle)
{
    // Welding: a closed sphere should have far fewer verts than 3*triangles.
    const int n = 32;
    auto g = sampleGrid(n, -1.f, 1.f, [](float x, float y, float z) {
        return 0.5f - std::sqrt(x * x + y * y + z * z);
    });
    auto m = MarchingCubes::extract(g.data(), n, n, n, 0.0f, {-1, -1, -1}, {1, 1, 1});
    ASSERT_GT(m.triangleCount, 0);
    EXPECT_LT(m.vertexCount, m.triangleCount * 3)
        << "shared edges should weld vertices, not emit 3 per triangle";
    // Euler characteristic V - E + F ≈ 2 for a genus-0 closed surface.
    const auto counts = edgeUseCounts(m);
    const int E = static_cast<int>(counts.size());
    const int chi = m.vertexCount - E + m.triangleCount;
    EXPECT_EQ(chi, 2) << "closed sphere should have Euler characteristic 2";
}
