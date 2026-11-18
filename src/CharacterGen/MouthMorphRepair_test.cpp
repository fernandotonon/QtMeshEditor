#include "MouthMorphRepair.h"

#include <gtest/gtest.h>

#include <cmath>

using namespace CharacterGen;

namespace {

// An n x n grid with 1 mm spacing in the xy plane, centred on the origin.
struct Grid {
    int n = 11;
    std::vector<float> positions;
    std::vector<int32_t> faces;
    Grid()
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
// The grid with the quad at the centre cell removed (its two triangles), and a 4 x 4 cell block removed next to it (a long loop).
std::vector<int32_t> gridWithHoles(const Grid& g, int bigFirstCell)
{
    std::vector<int32_t> out;
    const int cells = g.n - 1;
    for (int j = 0; j < cells; ++j)
        for (int i = 0; i < cells; ++i) {
            const bool small = (j == cells / 2 && i == cells / 2);
            const bool big = bigFirstCell >= 0 && i >= bigFirstCell && i < bigFirstCell + 4 && j >= 0 && j < 4;
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
    Grid g;
    const std::vector<int32_t> faces = gridWithHoles(g, 0);   // the small centre hole plus a 4 x 4 cell opening (16-vertex loop)
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
