/*
-----------------------------------------------------------------------------------
A QtMeshEditor file — DerivedMapOcclusion unit tests (Paint v2 Slice G, issue #550)

Pure-data: hand-builds DepthViews with synthetic flat depth images (the same
trick ProjectionPainter_test.cpp uses) so the visibility maths is checked without
rendering anything. No Ogre scene / GL.

Copyright (c) Fernando Tonon (https://github.com/fernandotonon)
The MIT License — see other project sources for the full header.
-----------------------------------------------------------------------------------
*/
#include <gtest/gtest.h>

#include "DerivedMapOcclusion.h"

#include <QColor>

#include <algorithm>
#include <cmath>

namespace {

// Orthographic viewProj looking down -Z from +Z, matching ProjectionPainter's
// test helper: x/y in [-halfExtent, halfExtent] map to NDC [-1, 1].
Ogre::Matrix4 orthoFromPlusZ(float eyeZ, float halfExtent, float zNear, float zFar)
{
    Ogre::Matrix4 view = Ogre::Matrix4::IDENTITY;
    view[2][2] = -1.0f;               // look toward -Z
    view[2][3] = eyeZ;
    Ogre::Matrix4 proj = Ogre::Matrix4::ZERO;
    proj[0][0] = 1.0f / halfExtent;
    proj[1][1] = 1.0f / halfExtent;
    proj[2][2] = -2.0f / (zFar - zNear);
    proj[2][3] = -(zFar + zNear) / (zFar - zNear);
    proj[3][3] = 1.0f;
    return proj * view;
}

// A depth image where every pixel encodes the SAME world distance.
QImage flatDepth(int size, float surfaceDist, float near, float far)
{
    const float g = 1.0f - (surfaceDist - near) / (far - near);   // near -> 1
    const int v = std::clamp(static_cast<int>(g * 255.0f + 0.5f), 0, 255);
    QImage img(size, size, QImage::Format_RGBA8888);
    img.fill(QColor(v, v, v, 255));
    return img;
}

// A view from +Z looking toward -Z, whose depth map says the nearest surface
// sits at `surfaceDist` from the eye.
DepthView viewFromPlusZ(float surfaceDist, float near = 4.0f, float far = 8.0f)
{
    DepthView v;
    v.viewProj = orthoFromPlusZ(5.0f, 2.0f, 0.1f, 20.0f);
    v.camPosition = Ogre::Vector3(0, 0, 5);
    v.camDirection = Ogre::Vector3(0, 0, -1);
    v.depthNear = near;
    v.depthFar = far;
    v.depth = flatDepth(32, surfaceDist, near, far);
    v.biasWorld = 0.05f;
    return v;
}

} // namespace

TEST(DerivedMapOcclusionTest, PointOnTheVisibleSurfaceIsVisible) {
    // Surface recorded at distance 5; the point at z=0 IS that surface.
    const DepthView v = viewFromPlusZ(5.0f);
    EXPECT_TRUE(DerivedMapOcclusion::isVisibleInView(Ogre::Vector3(0, 0, 0), v));
}

TEST(DerivedMapOcclusionTest, PointBehindTheSurfaceIsOccluded) {
    // Surface at distance 5, point at z=-2 (distance 7) is behind it.
    const DepthView v = viewFromPlusZ(5.0f);
    EXPECT_FALSE(DerivedMapOcclusion::isVisibleInView(Ogre::Vector3(0, 0, -2), v));
}

TEST(DerivedMapOcclusionTest, BiasPreventsSelfOcclusion) {
    // A point marginally behind the recorded surface (within bias) must still
    // count as visible — otherwise a surface occludes itself (depth acne) and
    // every vertex reads fully occluded.
    DepthView v = viewFromPlusZ(5.0f);
    const Ogre::Vector3 justBehind(0, 0, -0.02f);   // distance 5.02, bias 0.05
    EXPECT_TRUE(DerivedMapOcclusion::isVisibleInView(justBehind, v));
    v.biasWorld = 0.0f;
    EXPECT_FALSE(DerivedMapOcclusion::isVisibleInView(justBehind, v));
}

TEST(DerivedMapOcclusionTest, OffImagePointIsNotVisible) {
    const DepthView v = viewFromPlusZ(5.0f);
    // halfExtent is 2, so x=10 projects outside the depth image.
    EXPECT_FALSE(DerivedMapOcclusion::isVisibleInView(Ogre::Vector3(10, 0, 0), v));
}

TEST(DerivedMapOcclusionTest, PointBehindCameraIsNotVisible) {
    // z=50 is behind the eye (at z=5) looking toward -Z, so its camera-axis
    // distance is NEGATIVE. Note the perspective `behind` (w <= 0) flag cannot
    // catch this here: these fixtures use an ORTHOGRAPHIC viewProj where w stays
    // 1, so the sign of the axis distance is the only thing that rejects it.
    // Depth-map views in this feature are auto-framed orthographic-ish renders,
    // so that is the case that actually matters.
    const DepthView v = viewFromPlusZ(5.0f);
    EXPECT_FALSE(DerivedMapOcclusion::isVisibleInView(Ogre::Vector3(0, 0, 50), v));
}

TEST(DerivedMapOcclusionTest, NullDepthImageIsNotVisible) {
    DepthView v = viewFromPlusZ(5.0f);
    v.depth = QImage();
    EXPECT_FALSE(DerivedMapOcclusion::isVisibleInView(Ogre::Vector3(0, 0, 0), v));
}

TEST(DerivedMapOcclusionTest, ExposedPointHasZeroOcclusion) {
    // One view that can see the point, and the point is the visible surface.
    const std::vector<DepthView> views{viewFromPlusZ(5.0f)};
    const float occ = DerivedMapOcclusion::occlusionAt(
        Ogre::Vector3(0, 0, 0), Ogre::Vector3(0, 0, 1), views);
    EXPECT_NEAR(occ, 0.0f, 1e-5f);
}

TEST(DerivedMapOcclusionTest, BuriedPointIsFullyOccluded) {
    // The depth map says the nearest surface is at 5, but the point is at 7.
    const std::vector<DepthView> views{viewFromPlusZ(5.0f)};
    const float occ = DerivedMapOcclusion::occlusionAt(
        Ogre::Vector3(0, 0, -2), Ogre::Vector3(0, 0, 1), views);
    EXPECT_NEAR(occ, 1.0f, 1e-5f);
}

TEST(DerivedMapOcclusionTest, BackFacingViewsAreSkippedNotCountedAsOccluding) {
    // A view looking at the BACK of the surface must be ignored entirely. If it
    // were counted as "cannot see" the occlusion of a fully exposed vertex
    // would read ~0.5 instead of 0, darkening the whole mesh uniformly.
    DepthView behind = viewFromPlusZ(5.0f);
    behind.camPosition = Ogre::Vector3(0, 0, -5);
    behind.camDirection = Ogre::Vector3(0, 0, 1);      // looks toward +Z

    const std::vector<DepthView> views{viewFromPlusZ(5.0f), behind};
    const float occ = DerivedMapOcclusion::occlusionAt(
        Ogre::Vector3(0, 0, 0), Ogre::Vector3(0, 0, 1), views);  // normal +Z
    EXPECT_NEAR(occ, 0.0f, 1e-5f) << "the back-facing view must not contribute";
}

TEST(DerivedMapOcclusionTest, NoFacingViewYieldsZeroNotOne) {
    // If nothing faces the normal we know nothing — report unoccluded rather
    // than fully occluded, which would black out the map.
    DepthView behind = viewFromPlusZ(5.0f);
    behind.camPosition = Ogre::Vector3(0, 0, -5);
    behind.camDirection = Ogre::Vector3(0, 0, 1);
    const float occ = DerivedMapOcclusion::occlusionAt(
        Ogre::Vector3(0, 0, 0), Ogre::Vector3(0, 0, 1), {behind});
    EXPECT_NEAR(occ, 0.0f, 1e-5f);
}

TEST(DerivedMapOcclusionTest, EmptyViewSetIsUnoccluded) {
    EXPECT_NEAR(DerivedMapOcclusion::occlusionAt(
        Ogre::Vector3::ZERO, Ogre::Vector3::UNIT_Z, {}), 0.0f, 1e-6f);
}

TEST(DerivedMapOcclusionTest, PerVertexHelperMatchesPerPointCall) {
    const std::vector<DepthView> views{viewFromPlusZ(5.0f)};
    const std::vector<Ogre::Vector3> pos{{0, 0, 0}, {0, 0, -2}};
    const std::vector<Ogre::Vector3> nrm{{0, 0, 1}, {0, 0, 1}};
    const auto out = DerivedMapOcclusion::occlusionForVertices(pos, nrm, views);
    ASSERT_EQ(out.size(), 2u);
    EXPECT_NEAR(out[0], 0.0f, 1e-5f);
    EXPECT_NEAR(out[1], 1.0f, 1e-5f);
}

TEST(DerivedMapOcclusionTest, PerVertexHelperToleratesMissingNormals) {
    const std::vector<DepthView> views{viewFromPlusZ(5.0f)};
    const std::vector<Ogre::Vector3> pos{{0, 0, 0}, {0, 0, 0}};
    // Shorter normals array must not read out of bounds.
    const auto out = DerivedMapOcclusion::occlusionForVertices(pos, {}, views);
    EXPECT_EQ(out.size(), 2u);
}

TEST(DerivedMapOcclusionTest, SampleDirectionsAreUnitAndSpreadOverTheSphere) {
    const auto dirs = DerivedMapOcclusion::sampleDirections(32);
    ASSERT_EQ(dirs.size(), 32u);
    Ogre::Vector3 sum = Ogre::Vector3::ZERO;
    for (const auto& d : dirs) {
        EXPECT_NEAR(d.length(), 1.0f, 1e-4f);
        sum += d;
    }
    // Near-uniform over the sphere => the mean direction is near zero. A
    // pole-clustered lat/long grid would fail this and bias AO vertically.
    EXPECT_LT(sum.length() / 32.0f, 0.2f);
}

TEST(DerivedMapOcclusionTest, SampleDirectionsHandlesDegenerateCounts) {
    EXPECT_EQ(DerivedMapOcclusion::sampleDirections(1).size(), 1u);
    EXPECT_EQ(DerivedMapOcclusion::sampleDirections(0).size(), 1u);   // clamped
    EXPECT_EQ(DerivedMapOcclusion::sampleDirections(-5).size(), 1u);
    for (const auto& d : DerivedMapOcclusion::sampleDirections(1))
        EXPECT_NEAR(d.length(), 1.0f, 1e-4f);
}
