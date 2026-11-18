#include "MouthMorphRepair.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>

using namespace CharacterGen;

namespace {

// An n x n grid with 1 mm spacing in the xy plane, centred on the origin.
struct Grid {
    int n = 11;
    std::vector<float> positions;
    std::vector<int32_t> faces;
    explicit Grid(int size = 11) : n(size)
    {
        for (int j = 0; j < n; ++j)
            for (int i = 0; i < n; ++i) positions.insert(positions.end(), {(i - n / 2) * 0.001f, (j - n / 2) * 0.001f, 0.0f});
        for (int j = 0; j + 1 < n; ++j)
            for (int i = 0; i + 1 < n; ++i) {
                const int a = j * n + i, b = a + 1, c = a + n, d = c + 1;
                faces.insert(faces.end(), {a, b, d, a, d, c});
            }
    }
    int vertices() const { return n * n; }
    int centre() const { return (n / 2) * n + n / 2; }
};

const std::array<double, 3> kOrigin{0.0, 0.0, 0.0};

}  // namespace

TEST(MouthMorphRepair, ASpikedVertexIsReplacedByItsNeighbours)
{
    Grid g;
    std::vector<float> arkit(size_t(g.vertices()) * 3, 0.0f);
    arkit[size_t(g.centre()) * 3 + 1] = -1.0f;   // 1 cm down while every neighbour stays: the triangles around it tear
    ASSERT_GT(worstMouthStretch(g.positions, g.vertices(), g.faces, kOrigin, 1.0, arkit, 0), 5.0);
    EXPECT_GT(repairMouthMorphs(g.positions, g.vertices(), g.faces, kOrigin, 1.0, arkit, 1), 0);
    EXPECT_LT(worstMouthStretch(g.positions, g.vertices(), g.faces, kOrigin, 1.0, arkit, 0), 1.8);
    EXPECT_NEAR(arkit[size_t(g.centre()) * 3 + 1], 0.0f, 1e-3f);
}

TEST(MouthMorphRepair, ASmoothShapeIsLeftAlone)
{
    Grid g;
    std::vector<float> arkit(size_t(g.vertices()) * 3);
    for (int v = 0; v < g.vertices(); ++v) arkit[size_t(v) * 3 + 1] = -1.0f - 0.05f * float(v % g.n);   // a gentle shear, no tearing
    const std::vector<float> before = arkit;
    EXPECT_EQ(repairMouthMorphs(g.positions, g.vertices(), g.faces, kOrigin, 1.0, arkit, 1), 0);
    EXPECT_EQ(arkit, before);
}

TEST(MouthMorphRepair, FacesOutsideTheRadiusAreNotTouched)
{
    Grid g;
    std::vector<float> arkit(size_t(g.vertices()) * 3, 0.0f);
    arkit[size_t(g.centre()) * 3 + 1] = -1.0f;
    const std::vector<float> before = arkit;
    const std::array<double, 3> far{1.0, 1.0, 1.0};
    EXPECT_EQ(repairMouthMorphs(g.positions, g.vertices(), g.faces, far, 0.01, arkit, 1), 0);
    EXPECT_EQ(arkit, before);
}

namespace {
// The grid with the quad at the centre cell removed (its two triangles), and optionally a 4 x 4 cell block starting at cell
// (bigFirstCell, bigFirstCell) removed too (a long loop).
std::vector<int32_t> gridWithHoles(const Grid& g, int bigFirstCell)
{
    std::vector<int32_t> out;
    const int cells = g.n - 1;
    for (int j = 0; j < cells; ++j)
        for (int i = 0; i < cells; ++i) {
            const bool small = (j == cells / 2 && i == cells / 2);
            const bool big = bigFirstCell >= 0 && i >= bigFirstCell && i < bigFirstCell + 4 && j >= bigFirstCell && j < bigFirstCell + 4;
            if (small || big) continue;
            const size_t f = size_t(j * cells + i) * 6;
            out.insert(out.end(), g.faces.begin() + long(f), g.faces.begin() + long(f + 6));
        }
    return out;
}

// +1 / -1: the sign of the z normal of a triangle of the flat grid
int windingOf(const Grid& g, int32_t a, int32_t b, int32_t c)
{
    const float* p = g.positions.data();
    const float ux = p[b * 3] - p[a * 3], uy = p[b * 3 + 1] - p[a * 3 + 1];
    const float vx = p[c * 3] - p[a * 3], vy = p[c * 3 + 1] - p[a * 3 + 1];
    return ux * vy - uy * vx > 0 ? 1 : -1;
}
}  // namespace

TEST(MouthMorphRepair, ASmallHoleIsCappedWithTheSurroundingWinding)
{
    Grid g;
    const std::vector<int32_t> faces = gridWithHoles(g, -1);
    const std::vector<int32_t> caps = mouthHoleCaps(g.positions, g.vertices(), faces, kOrigin, 1.0);
    ASSERT_EQ(caps.size(), 6u);   // the missing quad, as two triangles
    const int surrounding = windingOf(g, g.faces[0], g.faces[1], g.faces[2]);
    for (size_t i = 0; i < caps.size(); i += 3) EXPECT_EQ(windingOf(g, caps[i], caps[i + 1], caps[i + 2]), surrounding);
}

TEST(MouthMorphRepair, ALongLoopAndHolesOutsideTheRadiusStayOpen)
{
    Grid g(13);   // 12 x 12 cells: the 4 x 4 block at cells 1..4 is enclosed and does not touch the centre hole (cell 6)
    const std::vector<int32_t> faces = gridWithHoles(g, 1);
    std::vector<size_t> sizes;
    for (const auto& l : boundaryLoops(faces, g.vertices())) sizes.push_back(l.size());
    std::sort(sizes.begin(), sizes.end());
    ASSERT_EQ(sizes, (std::vector<size_t>{4, 16, 48}));   // the small hole, the enclosed block, the outer rim
    EXPECT_EQ(mouthHoleCaps(g.positions, g.vertices(), faces, kOrigin, 1.0).size(), 6u);
    const std::array<double, 3> far{1.0, 1.0, 1.0};
    EXPECT_TRUE(mouthHoleCaps(g.positions, g.vertices(), faces, far, 0.01).empty());
}

TEST(MouthMorphRepair, HolesThatTouchAtAVertexAreBothCapped)
{
    Grid g;
    // remove two diagonal cells that share one corner: the boundary pinches there, so neither hole is a simple loop
    std::vector<int32_t> faces;
    const int cells = g.n - 1, c = cells / 2;
    for (int j = 0; j < cells; ++j)
        for (int i = 0; i < cells; ++i) {
            if ((i == c && j == c) || (i == c + 1 && j == c + 1)) continue;
            const size_t f = size_t(j * cells + i) * 6;
            faces.insert(faces.end(), g.faces.begin() + long(f), g.faces.begin() + long(f + 6));
        }
    const std::vector<int32_t> caps = mouthHoleCaps(g.positions, g.vertices(), faces, kOrigin, 1.0);
    ASSERT_EQ(caps.size(), 12u);
    const int surrounding = windingOf(g, g.faces[0], g.faces[1], g.faces[2]);
    for (size_t i = 0; i < caps.size(); i += 3) EXPECT_EQ(windingOf(g, caps[i], caps[i + 1], caps[i + 2]), surrounding);
}

TEST(MouthMorphRepair, CornerNormalsBlendFromOneSideToTheOther)
{
    Grid g;   // normals: the bottom row faces -y, the top row +y; the rows between are free and start wrong (+x)
    std::vector<float> normals(size_t(g.vertices()) * 3, 0.0f);
    std::vector<int32_t> inner;
    for (int j = 0; j < g.n; ++j)
        for (int i = 0; i < g.n; ++i) {
            const size_t v = size_t(j * g.n + i) * 3;
            if (j == 0) normals[v + 1] = -1.0f;
            else if (j == g.n - 1) normals[v + 1] = 1.0f;
            else { normals[v] = 1.0f; inner.push_back(j * g.n + i); }
        }
    blendCornerNormals(g.vertices(), g.faces, inner, normals, 2000);
    const auto y = [&](int j) { return normals[size_t(j * g.n + g.n / 2) * 3 + 1]; };
    EXPECT_NEAR(normals[size_t(g.centre()) * 3 + 1], 0.0f, 0.05f);       // the change is in the middle...
    for (int j = 1; j < g.n; ++j) EXPECT_GT(y(j), y(j - 1));                // ...and gradual
    for (int32_t v : inner) EXPECT_NEAR(std::hypot(normals[size_t(v) * 3], normals[size_t(v) * 3 + 1]), 1.0f, 1e-4f);
}

TEST(MouthMorphRepair, BoundaryLoopsFindTheHoleAndTheRim)
{
    Grid g;
    const std::vector<int32_t> faces = gridWithHoles(g, -1);   // the outer rim (40 vertices) and the one-cell hole (4)
    std::vector<size_t> sizes;
    for (const auto& l : boundaryLoops(faces, g.vertices())) sizes.push_back(l.size());
    std::sort(sizes.begin(), sizes.end());
    EXPECT_EQ(sizes, (std::vector<size_t>{4, 40}));
}

TEST(MouthMorphRepair, BridgeJoinsTwoLoopsCornerToCornerFacingTheAxis)
{
    // an inner ellipse (the lips' edge, 12 vertices) and a wider one 5 mm behind it (the lining's rim, 20), started at different
    // points and running in opposite directions
    const auto ellipse = [](int n, float a, float b, float z, int start, bool reverse) {
        std::vector<std::array<float, 3>> l;
        for (int i = 0; i < n; ++i) {
            const int k = reverse ? (start - i + n) % n : (start + i) % n;
            const float t = 6.2831853f * float(k) / float(n);
            l.push_back({a * std::cos(t), b * std::sin(t), z});
        }
        return l;
    };
    const auto A = ellipse(12, 0.024f, 0.006f, 0.0f, 3, false), B = ellipse(20, 0.026f, 0.010f, -0.005f, 7, true);
    const auto tris = bridgeLoops(A, B, {0.0, 0.0, 0.0});
    ASSERT_EQ(tris.size(), A.size() + B.size());
    for (const auto& t : tris) {
        int fromA = 0;
        for (int32_t v : t) fromA += v < int32_t(A.size()) ? 1 : 0;
        EXPECT_TRUE(fromA == 1 || fromA == 2);   // every triangle spans the gap
        const auto p = [&](int32_t v) { return v < int32_t(A.size()) ? A[size_t(v)] : B[size_t(v) - A.size()]; };
        const auto p0 = p(t[0]), p1 = p(t[1]), p2 = p(t[2]);
        const float e1[3] = {p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2]}, e2[3] = {p2[0] - p0[0], p2[1] - p0[1], p2[2] - p0[2]};
        const float n[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
        const float cx = (p0[0] + p1[0] + p2[0]) / 3.0f, cy = (p0[1] + p1[1] + p2[1]) / 3.0f;
        EXPECT_GE(-(n[0] * cx + n[1] * cy), 0.0f);   // faces the axis
        // no triangle crosses from the upper half to the lower half (the corners are matched)
        const bool up = p0[1] + p1[1] + p2[1] > 0.0f;
        for (const auto& q : {p0, p1, p2}) EXPECT_TRUE(std::fabs(q[1]) < 1e-4f || (q[1] > 0.0f) == up);
    }
}
