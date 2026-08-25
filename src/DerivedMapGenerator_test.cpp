/*
-----------------------------------------------------------------------------------
A QtMeshEditor file — DerivedMapGenerator unit tests (Paint v2 Slice G, issue #550)

Pure-data: builds synthetic EditableMeshes with KNOWN convex/concave geometry and
checks the cavity / curvature / AO signals land on the right sign and the right
UV texels. No Ogre scene / GL.

Copyright (c) Fernando Tonon (https://github.com/fernandotonon)
The MIT License — see other project sources for the full header.
-----------------------------------------------------------------------------------
*/
#include <gtest/gtest.h>

#include "DerivedMapGenerator.h"

#include <OgreVector2.h>
#include <OgreVector3.h>

#include <cmath>

namespace {

// A flat quad in the z=0 plane, normals +Z, UV filling 0..1. Baseline: every
// vertex must read as flat (concavity ~0).
EditableMesh flatQuad()
{
    EditableMesh m;
    m.subMeshes().resize(1);
    EditableSubMesh& sm = m.subMeshes()[0];
    const Ogre::Vector3 n(0, 0, 1);
    auto push = [&](float x, float y, float u, float v) {
        EditableVertex ev;
        ev.position = Ogre::Vector3(x, y, 0);
        ev.normal = n;      ev.hasNormal = true;
        ev.uv = Ogre::Vector2(u, v); ev.hasUV = true;
        sm.vertices.push_back(ev);
    };
    push(-1, -1, 0, 1); push(1, -1, 1, 1); push(1, 1, 1, 0); push(-1, 1, 0, 0);
    sm.triangles.push_back({0, 1, 2});
    sm.triangles.push_back({0, 2, 3});
    return m;
}

// A "valley": two quads meeting along the y axis, folded so the shared centre
// edge is CONCAVE (a crevice). The centre verts' normals point up-and-inward,
// so their 1-ring neighbours sit above the tangent plane => positive concavity.
EditableMesh valley()
{
    EditableMesh m;
    m.subMeshes().resize(1);
    EditableSubMesh& sm = m.subMeshes()[0];
    auto push = [&](const Ogre::Vector3& p, const Ogre::Vector3& n, float u, float v) {
        EditableVertex ev;
        ev.position = p;
        ev.normal = n.normalisedCopy(); ev.hasNormal = true;
        ev.uv = Ogre::Vector2(u, v);    ev.hasUV = true;
        sm.vertices.push_back(ev);
    };
    // Left wall rises to x=-1, centre trough at x=0 (z=0), right wall rises.
    const Ogre::Vector3 nUp(0, 0, 1);
    push({-1, -1, 1}, {-1, 0, 1}, 0.0f, 1.0f);   // 0 outer left, bottom
    push({-1,  1, 1}, {-1, 0, 1}, 0.0f, 0.0f);   // 1 outer left, top
    push({ 0, -1, 0}, nUp,        0.5f, 1.0f);   // 2 centre bottom (concave)
    push({ 0,  1, 0}, nUp,        0.5f, 0.0f);   // 3 centre top    (concave)
    push({ 1, -1, 1}, { 1, 0, 1}, 1.0f, 1.0f);   // 4 outer right, bottom
    push({ 1,  1, 1}, { 1, 0, 1}, 1.0f, 0.0f);   // 5 outer right, top
    sm.triangles.push_back({0, 2, 3});
    sm.triangles.push_back({0, 3, 1});
    sm.triangles.push_back({2, 4, 5});
    sm.triangles.push_back({2, 5, 3});
    return m;
}

// A "ridge": the mirror of valley() — centre edge pushed UP so it is CONVEX.
EditableMesh ridge()
{
    EditableMesh m = valley();
    EditableSubMesh& sm = m.subMeshes()[0];
    // Flip the profile: outer verts high -> low, centre low -> high.
    sm.vertices[0].position.z = 0.0f;
    sm.vertices[1].position.z = 0.0f;
    sm.vertices[2].position.z = 1.0f;
    sm.vertices[3].position.z = 1.0f;
    sm.vertices[4].position.z = 0.0f;
    sm.vertices[5].position.z = 0.0f;
    return m;
}

} // namespace

TEST(DerivedMapGeneratorTest, FlatSurfaceHasNoConcavity) {
    const auto c = DerivedMapGenerator::vertexConcavity(flatQuad());
    ASSERT_FALSE(c.empty());
    for (const float v : c) EXPECT_NEAR(v, 0.0f, 1e-4f);
}

TEST(DerivedMapGeneratorTest, ConcaveAndConvexHaveOppositeSign) {
    // The centre verts are the folded edge in both fixtures. A valley must
    // read positive (concave) there and a ridge negative (convex) — if these
    // ever agree in sign, cavity and edge-wear would target the same texels.
    const auto vc = DerivedMapGenerator::vertexConcavity(valley());
    const auto rc = DerivedMapGenerator::vertexConcavity(ridge());
    ASSERT_GE(vc.size(), 4u);
    ASSERT_EQ(vc.size(), rc.size());

    // Welding may reorder/merge, so find the extreme of each rather than
    // assuming index 2/3 survive as-is.
    const float vMax = *std::max_element(vc.begin(), vc.end());
    const float rMin = *std::min_element(rc.begin(), rc.end());
    EXPECT_GT(vMax, 0.05f) << "valley centre should read concave (positive)";
    EXPECT_LT(rMin, -0.05f) << "ridge centre should read convex (negative)";
}

TEST(DerivedMapGeneratorTest, CavityKeepsOnlyConcaveHalf) {
    DerivedMapGenerator::Options o;
    // Cavity discards ridges: a convex signal must clamp to 0, a concave one
    // must survive. Otherwise crevice dirt would also land on every edge.
    EXPECT_FLOAT_EQ(DerivedMapGenerator::remapForKind(-0.8f, DerivedMapKind::Cavity, o), 0.0f);
    EXPECT_FLOAT_EQ(DerivedMapGenerator::remapForKind(0.0f, DerivedMapKind::Cavity, o), 0.0f);
    EXPECT_NEAR(DerivedMapGenerator::remapForKind(0.6f, DerivedMapKind::Cavity, o), 0.6f, 1e-5f);
}

TEST(DerivedMapGeneratorTest, CurvatureIsSignedAroundNeutralHalf) {
    DerivedMapGenerator::Options o;
    o.flatTolerance = 0.02f;
    // Signed: concave above 0.5, convex below, flat pinned exactly to 0.5.
    EXPECT_NEAR(DerivedMapGenerator::remapForKind(0.0f, DerivedMapKind::Curvature, o), 0.5f, 1e-6f);
    EXPECT_NEAR(DerivedMapGenerator::remapForKind(0.01f, DerivedMapKind::Curvature, o), 0.5f, 1e-6f)
        << "within flatTolerance must pin to neutral (suppresses tessellation noise)";
    EXPECT_GT(DerivedMapGenerator::remapForKind(0.8f, DerivedMapKind::Curvature, o), 0.5f);
    EXPECT_LT(DerivedMapGenerator::remapForKind(-0.8f, DerivedMapKind::Curvature, o), 0.5f);
}

TEST(DerivedMapGeneratorTest, ContrastScalesBeforeClamping) {
    DerivedMapGenerator::Options o;
    o.contrast = 4.0f;
    // 0.2 * 4 = 0.8, and a large value must clamp rather than overflow.
    EXPECT_NEAR(DerivedMapGenerator::remapForKind(0.2f, DerivedMapKind::Cavity, o), 0.8f, 1e-5f);
    EXPECT_NEAR(DerivedMapGenerator::remapForKind(0.9f, DerivedMapKind::Cavity, o), 1.0f, 1e-5f);
}

TEST(DerivedMapGeneratorTest, RasteriseFillsUvSpaceAndReportsRange) {
    const EditableMesh m = flatQuad();
    const size_t n = DerivedMapGenerator::weldedVertexCount(m);
    ASSERT_GT(n, 0u);
    std::vector<float> per(n, 0.75f);

    DerivedMapGenerator::Options o;
    o.resolution = 32;
    o.dilationPixels = 0;
    DerivedMapGenerator::Report rep;
    const DerivedMap map = DerivedMapGenerator::rasterise(m, per, o, &rep);

    ASSERT_TRUE(rep.ok) << rep.error.toStdString();
    EXPECT_EQ(map.width, 32);
    EXPECT_EQ(map.height, 32);
    EXPECT_GT(rep.texelsRasterised, 0);
    // The quad's UV covers the whole unit square, so the centre must be
    // covered and carry the constant value.
    EXPECT_EQ(map.coverage[static_cast<size_t>(16) * 32 + 16], 1);
    EXPECT_NEAR(map.sample(0.5f, 0.5f), 0.75f, 1e-5f);
    EXPECT_NEAR(rep.minValue, 0.75f, 1e-5f);
    EXPECT_NEAR(rep.maxValue, 0.75f, 1e-5f);
}

TEST(DerivedMapGeneratorTest, RasteriseRejectsWrongSizedInput) {
    const EditableMesh m = flatQuad();
    DerivedMapGenerator::Options o;
    o.resolution = 8;
    DerivedMapGenerator::Report rep;
    // A mismatched per-vertex array must be refused, not read out of bounds.
    const DerivedMap map = DerivedMapGenerator::rasterise(m, {1.0f, 2.0f}, o, &rep);
    EXPECT_FALSE(rep.ok);
    EXPECT_FALSE(rep.error.isEmpty());
    EXPECT_TRUE(map.empty());
}

TEST(DerivedMapGeneratorTest, DilationExtendsBeyondCoverage) {
    // A half-width UV quad leaves the right half of the map uncovered; with
    // dilation on, texels just past the island edge must be filled (otherwise
    // bilinear/MIP sampling bleeds background across the seam).
    EditableMesh m = flatQuad();
    for (auto& v : m.subMeshes()[0].vertices) v.uv.x *= 0.5f;

    const size_t n = DerivedMapGenerator::weldedVertexCount(m);
    std::vector<float> per(n, 1.0f);

    DerivedMapGenerator::Options o;
    o.resolution = 32;
    o.dilationPixels = 0;
    DerivedMapGenerator::Report noDil;
    const DerivedMap a = DerivedMapGenerator::rasterise(m, per, o, &noDil);

    o.dilationPixels = 3;
    DerivedMapGenerator::Report withDil;
    const DerivedMap b = DerivedMapGenerator::rasterise(m, per, o, &withDil);

    ASSERT_TRUE(noDil.ok);
    ASSERT_TRUE(withDil.ok);
    EXPECT_EQ(noDil.texelsDilated, 0);
    EXPECT_GT(withDil.texelsDilated, 0);
    int covA = 0, covB = 0;
    for (size_t i = 0; i < a.coverage.size(); ++i) { covA += a.coverage[i]; covB += b.coverage[i]; }
    EXPECT_GT(covB, covA) << "dilation must grow the covered region";
}

TEST(DerivedMapGeneratorTest, GenerateCavityOnValleyProducesSignal) {
    DerivedMapGenerator::Options o;
    o.resolution = 64;
    o.contrast = 2.0f;
    DerivedMapGenerator::Report rep;
    const DerivedMap map = DerivedMapGenerator::generate(
        valley(), DerivedMapKind::Cavity, o, &rep);

    ASSERT_TRUE(rep.ok) << rep.error.toStdString();
    EXPECT_GT(rep.texelsRasterised, 0);
    // The trough runs down the middle of UV space (u=0.5) while the outer
    // walls are at u=0 / u=1, so the centre must be dirtier than the edge.
    const float centre = map.sample(0.5f, 0.5f);
    const float edge = map.sample(0.02f, 0.5f);
    EXPECT_GT(centre, edge) << "cavity should peak in the trough, not on the walls";
    EXPECT_GT(rep.maxValue, 0.0f);
}

TEST(DerivedMapGeneratorTest, GenerateRejectsAoAndPointsAtTheRightCall) {
    DerivedMapGenerator::Options o;
    DerivedMapGenerator::Report rep;
    // AO needs scene-side visibility; generate() must refuse rather than
    // silently emit an empty/garbage map.
    const DerivedMap map = DerivedMapGenerator::generate(
        flatQuad(), DerivedMapKind::AmbientOcclusion, o, &rep);
    EXPECT_FALSE(rep.ok);
    EXPECT_TRUE(rep.error.contains("fromVertexOcclusion"));
    EXPECT_TRUE(map.empty());
}

TEST(DerivedMapGeneratorTest, FromVertexOcclusionClampsAndRasterises) {
    const EditableMesh m = flatQuad();
    const size_t n = DerivedMapGenerator::weldedVertexCount(m);
    ASSERT_GT(n, 0u);
    // Out-of-range inputs must be clamped into 0..1, not stored raw.
    std::vector<float> occ(n, 2.5f);
    occ[0] = -1.0f;

    DerivedMapGenerator::Options o;
    o.resolution = 16;
    o.dilationPixels = 0;
    DerivedMapGenerator::Report rep;
    const DerivedMap map = DerivedMapGenerator::fromVertexOcclusion(m, occ, o, &rep);

    ASSERT_TRUE(rep.ok) << rep.error.toStdString();
    EXPECT_GE(rep.minValue, 0.0f);
    EXPECT_LE(rep.maxValue, 1.0f);
}

TEST(DerivedMapGeneratorTest, SampleClampsOutOfRangeUv) {
    const EditableMesh m = flatQuad();
    const size_t n = DerivedMapGenerator::weldedVertexCount(m);
    std::vector<float> per(n, 0.4f);
    DerivedMapGenerator::Options o;
    o.resolution = 8;
    o.dilationPixels = 0;
    const DerivedMap map = DerivedMapGenerator::rasterise(m, per, o, nullptr);
    ASSERT_FALSE(map.empty());
    // Must clamp, not index out of bounds.
    EXPECT_NEAR(map.sample(-5.0f, -5.0f), map.sample(0.0f, 0.0f), 1e-6f);
    EXPECT_NEAR(map.sample(9.0f, 9.0f), map.sample(0.999f, 0.999f), 1e-6f);
}

TEST(DerivedMapGeneratorTest, KindNamesAreStableForCachePaths) {
    // These strings become on-disk filename components — changing one silently
    // orphans every cached map, so pin them.
    EXPECT_STREQ(DerivedMapGenerator::kindName(DerivedMapKind::Cavity), "cavity");
    EXPECT_STREQ(DerivedMapGenerator::kindName(DerivedMapKind::Curvature), "curvature");
    EXPECT_STREQ(DerivedMapGenerator::kindName(DerivedMapKind::AmbientOcclusion), "ao");
    // Neutral backgrounds differ per kind: curvature's "no effect" is 0.5.
    EXPECT_FLOAT_EQ(DerivedMapGenerator::backgroundFor(DerivedMapKind::Curvature), 0.5f);
    EXPECT_FLOAT_EQ(DerivedMapGenerator::backgroundFor(DerivedMapKind::Cavity), 0.0f);
}
