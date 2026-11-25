// Generic segmentation core (#1179): BVH, mesh prep, raster, shape diameter,
// Leiden. Pure data: no Ogre, no GL, no model.

#include "GenericSeg/Leiden.h"
#include "GenericSeg/MeshRaster.h"
#include "GenericSeg/SegMesh.h"
#include "GenericSeg/ShapeDiameter.h"
#include "GenericSeg/TriangleBvh.h"
#include "GenericSeg/GenericSegTestMeshes.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <random>
#include <set>

using namespace GenericSeg;
using GenericSegTest::makeBox;
using GenericSegTest::makeTable;

// ---------------------------------------------------------------- BVH

TEST(GenericSegBvh, MatchesBruteForceOnRandomTriangles)
{
    std::mt19937 rng(7);
    std::uniform_real_distribution<float> u(-1.0f, 1.0f);
    std::vector<float> pos;
    std::vector<uint32_t> idx;
    for (int t = 0; t < 400; ++t) {
        const Vec3 c(u(rng), u(rng), u(rng));
        for (int k = 0; k < 3; ++k) {
            pos.insert(pos.end(), {c.x + 0.2f * u(rng), c.y + 0.2f * u(rng), c.z + 0.2f * u(rng)});
            idx.push_back(uint32_t(idx.size()));
        }
    }
    TriangleBvh bvh;
    bvh.build(pos.data(), int(pos.size() / 3), idx.data(), int(idx.size()));
    ASSERT_EQ(bvh.triangleCount(), 400);

    TriangleBvh single;   // brute force reference: one triangle at a time
    int hits = 0;
    for (int r = 0; r < 300; ++r) {
        const Vec3 o(2.5f * u(rng), 2.5f * u(rng), 2.5f * u(rng));
        const Vec3 d = normalized(Vec3(u(rng), u(rng), u(rng)) - o * 0.3f);
        float bestT = 1e9f;
        int bestF = -1;
        for (int t = 0; t < 400; ++t) {
            single.build(pos.data() + 9 * t, 3, std::vector<uint32_t>{0, 1, 2}.data(), 3);
            TriangleBvh::Hit h;
            if (single.intersect(o, d, 0.0f, 100.0f, h) && h.t < bestT) { bestT = h.t; bestF = t; }
        }
        TriangleBvh::Hit h;
        const bool hit = bvh.intersect(o, d, 0.0f, 100.0f, h);
        ASSERT_EQ(hit, bestF >= 0) << "ray " << r;
        if (hit) {
            ++hits;
            EXPECT_EQ(h.face, bestF);
            EXPECT_NEAR(h.t, bestT, 1e-4f);
        }
    }
    EXPECT_GT(hits, 20);   // the test must actually exercise hits
}

TEST(GenericSegBvh, IgnoreFaceSkipsTheOriginTriangle)
{
    const auto box = makeBox({-1, -1, -1}, {1, 1, 1}, 1);
    TriangleBvh bvh;
    bvh.build(box.positions.data(), box.vertexCount(), box.indices.data(), box.indexCount());
    // From the centre of a +X face straight inward: hits the -X side, 2 away.
    int plusX = -1;
    for (int f = 0; f < box.faceCount(); ++f)
        if (box.centroid(f)[0] > 0.99f) { plusX = f; break; }
    ASSERT_GE(plusX, 0);
    const auto c = box.centroid(plusX);
    TriangleBvh::Hit h;
    ASSERT_TRUE(bvh.intersect(Vec3(c[0], c[1], c[2]), Vec3(-1, 0, 0), 0.0f, 10.0f, h, plusX));
    EXPECT_NEAR(h.t, 2.0f, 1e-5f);
    EXPECT_GT(dot(bvh.faceNormal(h.face), Vec3(-1, 0, 0)), 0.99f);
}

// ---------------------------------------------------------------- SegMesh

TEST(GenericSegMesh, NormalisesAndWeldsSplitSides)
{
    const auto box = makeBox({10, 10, 10}, {12, 14, 16}, 2);
    const SegMesh m = SegMesh::build(box.positions.data(), box.vertexCount(), box.indices.data(), box.indexCount());
    ASSERT_EQ(m.faceCount(), box.faceCount());
    float maxR = 0;
    for (const Vec3& p : m.positions) maxR = std::max(maxR, length(p));
    EXPECT_NEAR(maxR, 1.0f, 1e-5f);
    // 6 sides of a 2x2 grid: 9 vertices each = 54 unwelded; welded box with
    // 2 segments per edge has 26 distinct points.
    EXPECT_EQ(box.vertexCount(), 54);
    EXPECT_EQ(m.weldedCount, 26);
    // Welded: every face of a closed box has exactly 3 edge neighbours.
    for (int f = 0; f < m.faceCount(); ++f)
        EXPECT_EQ(m.faceNeighbors[f].size(), 3u) << "face " << f;
}

TEST(GenericSegMesh, RejectsInvalidInput)
{
    std::vector<float> pos{0, 0, 0, 1, 0, 0, 0, 1, 0};
    std::vector<uint32_t> bad{0, 1, 7};
    EXPECT_EQ(SegMesh::build(pos.data(), 3, bad.data(), 3).faceCount(), 0);
    EXPECT_EQ(SegMesh::build(nullptr, 0, nullptr, 0).faceCount(), 0);
}

// ---------------------------------------------------------------- Raster

TEST(GenericSegRaster, FaceIdsAndDepthOrder)
{
    // Two parallel squares facing +Z, the front one (z = 0.5) half the size.
    GenericSegTest::TestMesh m;
    auto quad = [&](float s, float z) {
        const uint32_t b = uint32_t(m.positions.size() / 3);
        m.positions.insert(m.positions.end(), {-s, -s, z, s, -s, z, s, s, z, -s, s, z});
        m.indices.insert(m.indices.end(), {b, b + 1, b + 2, b, b + 2, b + 3});
    };
    quad(0.6f, -0.5f);   // faces 0, 1 (back)
    quad(0.3f, 0.5f);    // faces 2, 3 (front)
    const SegMesh mesh = SegMesh::build(m.positions.data(), m.vertexCount(), m.indices.data(), m.indexCount());

    const Raster front = rasterize(mesh, OrthoCamera::lookingFrom(Vec3(0, 0, 1)), 64);
    EXPECT_GE(front.at(32, 32), 2);        // centre: the front square
    EXPECT_LE(front.at(32, 32), 3);
    EXPECT_EQ(front.at(0, 0), -1);         // corner: background
    const Raster back = rasterize(mesh, OrthoCamera::lookingFrom(Vec3(0, 0, -1)), 64);
    EXPECT_LE(back.at(32, 32), 1);         // from behind, the big square is in front

    const auto px = front.pixelsPerFace(mesh.faceCount());
    EXPECT_GT(px[2] + px[3], 0);
    EXPECT_GT(px[0] + px[1], 0);           // the ring of the back square around the front one
}

TEST(GenericSegRaster, ViewDirectionsAreUnitAndDistinct)
{
    for (int count : {12, 20, 7}) {
        const auto dirs = viewDirections(count);
        ASSERT_EQ(int(dirs.size()), count);
        for (size_t i = 0; i < dirs.size(); ++i) {
            EXPECT_NEAR(length(dirs[i]), 1.0f, 1e-4f);
            for (size_t j = i + 1; j < dirs.size(); ++j)
                EXPECT_LT(dot(dirs[i], dirs[j]), 0.999f);
        }
    }
}

TEST(GenericSegRaster, ShadedImagesHaveBlackBackground)
{
    const auto box = makeBox({-1, -1, -1}, {1, 1, 1}, 2);
    const SegMesh mesh = SegMesh::build(box.positions.data(), box.vertexCount(), box.indices.data(), box.indexCount());
    const OrthoCamera cam = OrthoCamera::lookingFrom(Vec3(1, 1, 1));
    const Raster r = rasterize(mesh, cam, 48);
    for (const QImage& img : {shadeNormals(r, mesh), shadeMatte(r, mesh, cam),
                              shadeFaceScalar(r, std::vector<float>(mesh.faceCount(), 0.5f))}) {
        ASSERT_EQ(img.width(), 48);
        EXPECT_EQ(img.pixel(0, 0) & 0xFFFFFF, 0u);
        EXPECT_NE(img.pixel(24, 24) & 0xFFFFFF, 0u);
    }
}

// ---------------------------------------------------------------- Shape diameter

namespace {
// Median SDF of a slab's big faces (away from the rim), in input units.
float slabSdf(float thickness)
{
    const float h = 0.5f * thickness;
    const auto slab = makeBox({-1, -h, -1}, {1, h, 1}, 6);
    const SegMesh mesh = SegMesh::build(slab.positions.data(), slab.vertexCount(), slab.indices.data(), slab.indexCount());
    TriangleBvh bvh;
    std::vector<float> flat;
    for (const Vec3& p : mesh.positions) flat.insert(flat.end(), {p.x, p.y, p.z});
    bvh.build(flat.data(), int(mesh.positions.size()), mesh.indices.data(), int(mesh.indices.size()));
    SdfOptions o;
    o.smoothIterations = 0;
    const auto sdf = shapeDiameter(mesh, bvh, o);
    std::vector<float> big;
    for (int f = 0; f < mesh.faceCount(); ++f)
        if (std::fabs(mesh.faceNormals[f].y) > 0.99f) {
            const auto c = slab.centroid(f);
            if (std::fabs(c[0]) < 0.6f && std::fabs(c[2]) < 0.6f) big.push_back(sdf[f]);
        }
    if (big.empty()) return -1.0f;
    std::sort(big.begin(), big.end());
    return big[big.size() / 2] * mesh.scale;
}
} // namespace

TEST(GenericSegSdf, SlabThicknessIsProportional)
{
    // Shapira's estimator averages slanted rays in a 120 degree cone, so a flat
    // slab reads about 1/mean(cos) = 1.33x its thickness. The segmenter only
    // uses SDF relatively, so pin the bias band and the proportionality.
    const float thin = slabSdf(0.2f), thick = slabSdf(0.4f);
    ASSERT_GT(thin, 0.0f);
    EXPECT_GT(thin, 0.2f);
    EXPECT_LT(thin, 0.2f * 1.45f);
    EXPECT_NEAR(thick / thin, 2.0f, 0.2f);
}

TEST(GenericSegSdf, OpenSheetHasNoVolume)
{
    std::vector<float> pos{-1, 0, -1, 1, 0, -1, 1, 0, 1, -1, 0, 1};
    std::vector<uint32_t> idx{0, 2, 1, 0, 3, 2};
    const SegMesh mesh = SegMesh::build(pos.data(), 4, idx.data(), 6);
    TriangleBvh bvh;
    std::vector<float> flat;
    for (const Vec3& p : mesh.positions) flat.insert(flat.end(), {p.x, p.y, p.z});
    bvh.build(flat.data(), 4, mesh.indices.data(), 6);
    for (float v : shapeDiameter(mesh, bvh)) EXPECT_EQ(v, 0.0f);
}

TEST(GenericSegSdf, NormalizeKeepsOrderInUnitRange)
{
    const auto n = normalizeSdf({0.1f, 0.4f, 0.2f, 0.9f});
    EXPECT_FLOAT_EQ(n[0], 0.0f);
    EXPECT_FLOAT_EQ(n[3], 1.0f);
    EXPECT_LT(n[2], n[1]);
    EXPECT_EQ(normalizeSdf({0.5f, 0.5f}), (std::vector<float>{0.0f, 0.0f}));
}

TEST(GenericSegSdf, NormalizeIgnoresAFewOutliers)
{
    // 1000 faces spread over 0.1..0.2 plus 3 huge ones: the spread must still
    // use most of the range (min..max scaling squashed it into ~0.02).
    std::vector<float> raw;
    for (int i = 0; i < 1000; ++i) raw.push_back(0.1f + 0.1f * float(i) / 999.0f);
    raw.insert(raw.end(), {50.0f, 60.0f, 70.0f});
    const auto n = normalizeSdf(raw);
    EXPECT_LT(n[10], 0.1f);
    EXPECT_GT(n[985], 0.9f);
    EXPECT_FLOAT_EQ(n[1002], 1.0f);   // outliers clamp to the top
}

// ---------------------------------------------------------------- Leiden

namespace {
void addClique(std::vector<GraphEdge>& e, int first, int size, float w = 1.0f)
{
    for (int i = 0; i < size; ++i)
        for (int j = i + 1; j < size; ++j) e.push_back({first + i, first + j, w});
}
} // namespace

TEST(GenericSegLeiden, TwoCliquesWithABridge)
{
    std::vector<GraphEdge> e;
    addClique(e, 0, 6);
    addClique(e, 6, 6);
    e.push_back({5, 6, 1.0f});
    const auto c = leiden(12, e);
    for (int i = 1; i < 6; ++i) EXPECT_EQ(c[i], c[0]);
    for (int i = 7; i < 12; ++i) EXPECT_EQ(c[i], c[6]);
    EXPECT_NE(c[0], c[6]);
}

TEST(GenericSegLeiden, RingOfCliquesAndDeterminism)
{
    std::vector<GraphEdge> e;
    const int k = 8, s = 5;
    for (int q = 0; q < k; ++q) {
        addClique(e, q * s, s);
        e.push_back({q * s, ((q + 1) % k) * s + 1, 1.0f});
    }
    const auto c = leiden(k * s, e);
    std::set<int> distinct(c.begin(), c.end());
    EXPECT_EQ(int(distinct.size()), k);
    for (int q = 0; q < k; ++q)
        for (int i = 1; i < s; ++i) EXPECT_EQ(c[q * s + i], c[q * s]);
    EXPECT_EQ(leiden(k * s, e), c);
    // The found partition beats putting everything together or everything apart.
    std::vector<int> one(k * s, 0), apart(k * s);
    for (int i = 0; i < k * s; ++i) apart[i] = i;
    EXPECT_GT(modularity(k * s, e, c), modularity(k * s, e, one));
    EXPECT_GT(modularity(k * s, e, c), modularity(k * s, e, apart));
}

TEST(GenericSegLeiden, CpmHasNoResolutionLimit)
{
    // Many small cliques in a ring, each also loosely tied to its neighbours
    // by a sparse "ring" structure inside: modularity on a big graph tends to
    // cut sparse groups; CPM keeps every group whose density beats gamma.
    std::vector<GraphEdge> e;
    const int groups = 30, s = 10;
    for (int q = 0; q < groups; ++q) {
        // A sparse group: a ring of 10 with chords to the next two (degree 4).
        for (int i = 0; i < s; ++i) {
            e.push_back({q * s + i, q * s + (i + 1) % s, 1.0f});
            e.push_back({q * s + i, q * s + (i + 2) % s, 1.0f});
        }
        e.push_back({q * s, ((q + 1) % groups) * s + 5, 1.0f});   // one bridge
    }
    LeidenOptions o;
    o.quality = LeidenQuality::Cpm;
    o.resolution = 0.1;   // groups have density 20 / 45 = 0.44
    const auto c = leiden(groups * s, e, o);
    std::set<int> distinct(c.begin(), c.end());
    EXPECT_EQ(int(distinct.size()), groups);
    for (int q = 0; q < groups; ++q)
        for (int i = 1; i < s; ++i) EXPECT_EQ(c[q * s + i], c[q * s]) << "group " << q;
}

TEST(GenericSegLeiden, ResolutionZeroGivesComponentsAndIsolatedNodesStayAlone)
{
    std::vector<GraphEdge> e;
    addClique(e, 0, 4);
    addClique(e, 4, 4);
    e.push_back({3, 4, 1.0f});
    LeidenOptions o;
    o.resolution = 0.0;
    const auto c = leiden(10, e, o);   // nodes 8 and 9 have no edges
    for (int i = 1; i < 8; ++i) EXPECT_EQ(c[i], c[0]);
    EXPECT_NE(c[8], c[0]);
    EXPECT_NE(c[9], c[0]);
    EXPECT_NE(c[8], c[9]);
    EXPECT_TRUE(leiden(0, {}).empty());
}
