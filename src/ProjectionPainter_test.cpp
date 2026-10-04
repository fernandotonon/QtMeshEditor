/*
-----------------------------------------------------------------------------------
A QtMeshEditor file — ProjectionPainter unit tests (Paint v2 Slice F, issue #549)

Pure-data: builds synthetic world triangles + view/proj matrices + source
QImages and asserts the projection/occlusion/stencil math. No Ogre scene / GL.

Copyright (c) Fernando Tonon (https://github.com/fernandotonon)
The MIT License — see other project sources for the full header.
-----------------------------------------------------------------------------------
*/
#include <gtest/gtest.h>

#include "ProjectionPainter.h"
#include "TexturePaintBuffer.h"

#include <QImage>

#include <OgreMatrix4.h>
#include <OgreVector2.h>
#include <OgreVector3.h>

#include <cmath>

namespace {

// An orthographic-ish view*proj looking along -Z from +Z, mapping world
// [-1,1]^2 in X/Y to NDC [-1,1] and world Z in [zNear..zFar] to NDC z. Simple,
// invertible, and enough to exercise the projection + occlusion paths.
Ogre::Matrix4 orthoViewProj(float eyeZ = 5.0f, float halfExtent = 1.0f,
                            float zNear = 0.1f, float zFar = 10.0f)
{
    // View: translate world so the eye at +eyeZ looks toward -Z. Camera space
    // z' = eyeZ - worldZ (distance in front grows positive).
    Ogre::Matrix4 view = Ogre::Matrix4::IDENTITY;
    view[2][2] = -1.0f;        // flip Z (look along -Z)
    view[2][3] = eyeZ;         // z_cam = eyeZ - z_world
    // Ortho proj: X/Y by 1/halfExtent; Z linear to [-1,1] over [zNear,zFar]; w=1.
    Ogre::Matrix4 proj = Ogre::Matrix4::ZERO;
    proj[0][0] = 1.0f / halfExtent;
    proj[1][1] = 1.0f / halfExtent;
    proj[2][2] = 2.0f / (zFar - zNear);
    proj[2][3] = -(zFar + zNear) / (zFar - zNear);
    proj[3][3] = 1.0f;
    return proj * view;
}

// A unit quad in the Z=z plane spanning [-1,1] in X/Y, UV0 filling [0,1],
// normal +Z (facing the +Z camera). Two triangles.
std::vector<ProjectionPainter::Triangle> frontQuad(float z, bool faceCamera = true)
{
    const Ogre::Vector3 n = faceCamera ? Ogre::Vector3(0, 0, 1) : Ogre::Vector3(0, 0, -1);
    ProjectionPainter::Triangle t0, t1;
    // positions
    const Ogre::Vector3 p00(-1, -1, z), p10(1, -1, z), p11(1, 1, z), p01(-1, 1, z);
    // UV (top-left origin, V down): map +Y(world) to v=0 top
    const Ogre::Vector2 u00(0, 1), u10(1, 1), u11(1, 0), u01(0, 0);
    t0.p[0] = p00; t0.p[1] = p10; t0.p[2] = p11; t0.uv[0] = u00; t0.uv[1] = u10; t0.uv[2] = u11; t0.normal = n;
    t1.p[0] = p00; t1.p[1] = p11; t1.p[2] = p01; t1.uv[0] = u00; t1.uv[1] = u11; t1.uv[2] = u01; t1.normal = n;
    return { t0, t1 };
}

QImage solid(int size, QColor c) {
    QImage img(size, size, QImage::Format_RGBA8888);
    img.fill(c);
    return img;
}

int opaqueTexels(const TexturePaintBuffer& b) {
    int n = 0;
    for (int y = 0; y < b.height(); ++y)
        for (int x = 0; x < b.width(); ++x)
            if (b.pixel(x, y).a > 0.5f) ++n;
    return n;
}

} // namespace

TEST(ProjectionPainterTest, FrontQuadProjectsOpaque) {
    auto tris = frontQuad(0.0f, /*faceCamera*/true);
    ProjectionPainter::View v{ orthoViewProj(), Ogre::Vector3(0, 0, -1), Ogre::Vector3(0, 0, 5) };
    TexturePaintBuffer out; out.resize(64, 64);
    ProjectionPainter::Options opts; opts.resolution = 64; opts.backfaceCull = true;
    auto rep = ProjectionPainter::project(tris, v, solid(32, Qt::red), out, opts);
    ASSERT_TRUE(rep.ok) << rep.error.toStdString();
    EXPECT_GT(rep.texelsWritten, 0);
    EXPECT_GT(opaqueTexels(out), 0);
    // A centre texel should be opaque red.
    const Ogre::ColourValue c = out.pixel(32, 32);
    EXPECT_GT(c.a, 0.5f);
    EXPECT_GT(c.r, 0.5f);
    EXPECT_LT(c.g, 0.5f);
}

TEST(ProjectionPainterTest, BackfaceQuadCulled) {
    auto tris = frontQuad(0.0f, /*faceCamera*/false);   // normal points AWAY from camera
    ProjectionPainter::View v{ orthoViewProj(), Ogre::Vector3(0, 0, -1), Ogre::Vector3(0, 0, 5) };
    TexturePaintBuffer out; out.resize(64, 64);
    ProjectionPainter::Options opts; opts.resolution = 64; opts.backfaceCull = true;
    auto rep = ProjectionPainter::project(tris, v, solid(32, Qt::red), out, opts);
    ASSERT_TRUE(rep.ok);
    EXPECT_EQ(rep.texelsWritten, 0);
    EXPECT_GT(rep.texelsBackface, 0);
    EXPECT_EQ(opaqueTexels(out), 0);
}

TEST(ProjectionPainterTest, StencilAlphaGatesWrite) {
    auto tris = frontQuad(0.0f, true);
    ProjectionPainter::View v{ orthoViewProj(), Ogre::Vector3(0, 0, -1), Ogre::Vector3(0, 0, 5) };
    // Source: left half opaque, right half fully transparent.
    QImage src(32, 32, QImage::Format_RGBA8888);
    for (int y = 0; y < 32; ++y)
        for (int x = 0; x < 32; ++x)
            src.setPixelColor(x, y, x < 16 ? QColor(0, 255, 0, 255) : QColor(0, 255, 0, 0));
    TexturePaintBuffer out; out.resize(64, 64);
    ProjectionPainter::Options opts; opts.resolution = 64;
    auto rep = ProjectionPainter::project(tris, v, src, out, opts);
    ASSERT_TRUE(rep.ok);
    // Left half (u<0.5) should be opaque, right half transparent.
    EXPECT_GT(out.pixel(16, 32).a, 0.5f);
    EXPECT_LT(out.pixel(48, 32).a, 0.5f);
}

// --- F-B: occlusion + depth-limit (hand-built OcclusionMap, no GL) ---

namespace {
// A depth map (near=bright/far=dark) where every pixel encodes the SAME world
// distance `surfaceDist`, over the range [depthNear, depthFar]. Simulates a
// flat occluder at surfaceDist filling the view.
QImage flatDepthMap(int size, float surfaceDist, float depthNear, float depthFar) {
    const float g = 1.0f - (surfaceDist - depthNear) / (depthFar - depthNear); // near→1
    const int v = std::clamp(static_cast<int>(g * 255.0f + 0.5f), 0, 255);
    QImage img(size, size, QImage::Format_RGBA8888);
    img.fill(QColor(v, v, v, 255));
    return img;
}
} // namespace

TEST(ProjectionPainterTest, OccludedFarSurfaceRejected) {
    // Camera at +Z=5 looking -Z. Depth map says the nearest visible surface is
    // at distance 5 (the z=0 plane). A quad at z=-1 is at distance 6 → behind →
    // must be rejected as occluded; a quad at z=0 (distance 5) writes through.
    const float near = 4.0f, far = 6.0f;
    ProjectionPainter::View v{ orthoViewProj(), Ogre::Vector3(0, 0, -1), Ogre::Vector3(0, 0, 5) };
    ProjectionPainter::OcclusionMap occ;
    occ.viewProj = orthoViewProj();
    occ.camPosition = Ogre::Vector3(0, 0, 5);
    occ.camDirection = Ogre::Vector3(0, 0, -1);
    occ.depthNear = near; occ.depthFar = far;
    occ.depth = flatDepthMap(64, /*surfaceDist*/5.0f, near, far);  // z=0 plane
    occ.biasWorld = 0.05f;

    ProjectionPainter::Options opts; opts.resolution = 64; opts.useOcclusion = true;

    // Far quad (z=-1, distance 6) — occluded.
    { TexturePaintBuffer out; out.resize(64, 64);
      auto rep = ProjectionPainter::project(frontQuad(-1.0f, true), v, solid(32, Qt::red), out, opts, &occ);
      ASSERT_TRUE(rep.ok);
      EXPECT_EQ(rep.texelsWritten, 0);
      EXPECT_GT(rep.texelsOccluded, 0);
      EXPECT_EQ(opaqueTexels(out), 0); }

    // Near quad (z=0, distance 5 == surface) — writes through (within bias).
    { TexturePaintBuffer out; out.resize(64, 64);
      auto rep = ProjectionPainter::project(frontQuad(0.0f, true), v, solid(32, Qt::red), out, opts, &occ);
      ASSERT_TRUE(rep.ok);
      EXPECT_GT(rep.texelsWritten, 0);
      EXPECT_EQ(rep.texelsOccluded, 0) << "self-projection should not acne-occlude"; }
}

TEST(ProjectionPainterTest, DepthLimitCullsBeyondNearestSurface) {
    // useOcclusion off, depthLimit on: reject texels more than `depthLimit`
    // world units behind the nearest visible surface. Surface at distance 5;
    // a quad at distance 6 with depthLimit 0.5 → culled; with depthLimit 2 → kept.
    const float near = 4.0f, far = 7.0f;
    ProjectionPainter::View v{ orthoViewProj(), Ogre::Vector3(0, 0, -1), Ogre::Vector3(0, 0, 5) };
    ProjectionPainter::OcclusionMap occ;
    occ.viewProj = orthoViewProj();
    occ.camPosition = Ogre::Vector3(0, 0, 5);
    occ.camDirection = Ogre::Vector3(0, 0, -1);
    occ.depthNear = near; occ.depthFar = far;
    occ.depth = flatDepthMap(64, 5.0f, near, far);
    // A REALISTIC anti-acne bias (not a 1e6 sentinel): with `useOcclusion` off
    // the occlusion reject must not fire at all, so the depth limit alone
    // decides. A large bias here would hide an ordering regression in
    // classifyDepth instead of catching it.
    occ.biasWorld = 0.05f;

    ProjectionPainter::Options opts; opts.resolution = 64; opts.useOcclusion = false;

    { opts.depthLimit = 0.5f; TexturePaintBuffer out; out.resize(64, 64);
      auto rep = ProjectionPainter::project(frontQuad(-1.0f, true), v, solid(32, Qt::red), out, opts, &occ);
      ASSERT_TRUE(rep.ok);
      EXPECT_GT(rep.texelsDepthCulled, 0);
      EXPECT_EQ(rep.texelsOccluded, 0);  // Occlude is OFF — must not cull
      EXPECT_EQ(rep.texelsWritten, 0); }

    { opts.depthLimit = 2.0f; TexturePaintBuffer out; out.resize(64, 64);
      auto rep = ProjectionPainter::project(frontQuad(-1.0f, true), v, solid(32, Qt::red), out, opts, &occ);
      ASSERT_TRUE(rep.ok);
      EXPECT_EQ(rep.texelsOccluded, 0);  // Occlude is OFF — must not cull
      EXPECT_GT(rep.texelsWritten, 0); }
}

TEST(ProjectionPainterTest, DepthLimitOffAndOcclusionOffWritesThrough) {
    // Both toggles off: a texel 1 unit behind the nearest visible surface must
    // write through untouched. Guards against either test reintroducing an
    // unconditional occlusion reject.
    const float near = 4.0f, far = 7.0f;
    ProjectionPainter::View v{ orthoViewProj(), Ogre::Vector3(0, 0, -1), Ogre::Vector3(0, 0, 5) };
    ProjectionPainter::OcclusionMap occ;
    occ.viewProj = orthoViewProj();
    occ.camPosition = Ogre::Vector3(0, 0, 5);
    occ.camDirection = Ogre::Vector3(0, 0, -1);
    occ.depthNear = near; occ.depthFar = far;
    occ.depth = flatDepthMap(64, 5.0f, near, far);
    occ.biasWorld = 0.05f;

    ProjectionPainter::Options opts; opts.resolution = 64;
    opts.useOcclusion = false; opts.depthLimit = 0.0f;

    TexturePaintBuffer out; out.resize(64, 64);
    auto rep = ProjectionPainter::project(frontQuad(-1.0f, true), v, solid(32, Qt::red), out, opts, &occ);
    ASSERT_TRUE(rep.ok);
    EXPECT_EQ(rep.texelsOccluded, 0);
    EXPECT_EQ(rep.texelsDepthCulled, 0);
    EXPECT_GT(rep.texelsWritten, 0);
}

TEST(ProjectionPainterTest, DabPaintsWithinFootprintAndAccumulates) {
    auto tris = frontQuad(0.0f, true);
    ProjectionPainter::View v{ orthoViewProj(), Ogre::Vector3(0, 0, -1), Ogre::Vector3(0, 0, 5) };
    TexturePaintBuffer out; out.resize(64, 64);
    out.clear(Ogre::ColourValue(0, 0, 0, 0));
    ProjectionPainter::Options opts;
    const Ogre::Vector2 centre(0.5f, 0.5f);
    const int n1 = ProjectionPainter::projectDab(tris, v, QImage(), centre, 0.15f,
                                                 Ogre::ColourValue(0, 0, 1, 1), 0.5f, out, opts);
    EXPECT_GT(n1, 0);
    const float aAfter1 = out.pixel(32, 32).a;
    EXPECT_GT(aAfter1, 0.0f);
    // A corner far outside the footprint stays untouched.
    EXPECT_LT(out.pixel(2, 2).a, 1e-3f);
    // A second dab at the same spot accumulates (alpha increases).
    ProjectionPainter::projectDab(tris, v, QImage(), centre, 0.15f,
                                  Ogre::ColourValue(0, 0, 1, 1), 0.5f, out, opts);
    EXPECT_GT(out.pixel(32, 32).a, aAfter1);
}

// --- non-square targets (the "decal commits as an all-white layer" bug) -----

TEST(ProjectionPainterTest, NonSquareTargetKeepsItsAspect) {
    // A generated diffuse is routinely non-square (e.g. 2504x2526). The
    // projection must fill the caller's buffer at ITS size — a square output
    // gets resize()d downstream, which fills opaque white and drops the decal.
    auto tris = frontQuad(0.0f, /*faceCamera*/true);
    ProjectionPainter::View v{ orthoViewProj(), Ogre::Vector3(0, 0, -1),
                               Ogre::Vector3(0, 0, 5) };
    TexturePaintBuffer out; out.resize(64, 96);
    ProjectionPainter::Options opts;    // resolution left 0 = keep buffer size
    opts.backfaceCull = true;
    auto rep = ProjectionPainter::project(tris, v, solid(32, Qt::red), out, opts);
    ASSERT_TRUE(rep.ok) << rep.error.toStdString();
    EXPECT_EQ(out.width(), 64);
    EXPECT_EQ(out.height(), 96) << "a square output would be resized to white "
                                   "by PaintLayerStack::addFromBuffer";
    EXPECT_GT(rep.texelsWritten, 0);
    // The quad covers UV [0..1] on both axes, so the full height is painted:
    // a square-projected result would leave the bottom third untouched.
    EXPECT_GT(out.pixel(32, 90).a, 0.5f)
        << "bottom rows must be covered on a non-square target";
    EXPECT_GT(out.pixel(32, 48).r, 0.5f);
}

TEST(ProjectionPainterTest, ExplicitSquareResolutionStillHonoured) {
    // opts.resolution is a square request — keep that contract for callers
    // (e.g. bakes) that deliberately ask for a fixed square size.
    auto tris = frontQuad(0.0f, /*faceCamera*/true);
    ProjectionPainter::View v{ orthoViewProj(), Ogre::Vector3(0, 0, -1),
                               Ogre::Vector3(0, 0, 5) };
    TexturePaintBuffer out; out.resize(10, 200);
    ProjectionPainter::Options opts; opts.resolution = 48;
    auto rep = ProjectionPainter::project(tris, v, solid(32, Qt::red), out, opts);
    ASSERT_TRUE(rep.ok) << rep.error.toStdString();
    EXPECT_EQ(out.width(), 48);
    EXPECT_EQ(out.height(), 48);
}

TEST(ProjectionPainterTest, DabOnNonSquareTargetStaysInBounds) {
    auto tris = frontQuad(0.0f, /*faceCamera*/true);
    ProjectionPainter::View v{ orthoViewProj(), Ogre::Vector3(0, 0, -1),
                               Ogre::Vector3(0, 0, 5) };
    TexturePaintBuffer out; out.resize(64, 96);
    out.clear(Ogre::ColourValue(0, 0, 0, 0));
    ProjectionPainter::Options opts;
    const int n = ProjectionPainter::projectDab(
        tris, v, QImage(), Ogre::Vector2(0.5f, 0.5f), 0.25f,
        Ogre::ColourValue(1, 0, 0, 1), 1.0f, out, opts);
    EXPECT_GT(n, 0);
    EXPECT_EQ(out.width(), 64);
    EXPECT_EQ(out.height(), 96);
    // Centre of a v=0.5 dab lands at y = 0.5*96 = 48, not 0.5*64 = 32.
    EXPECT_GT(out.pixel(32, 48).a, 0.5f) << "dab must centre on the V axis "
                                            "scaled by HEIGHT";

    // brushRadiusUv is a UV radius, so the footprint must span that same
    // fraction on BOTH axes: 0.25 * 96 = 24 px vertically (a radius taken
    // from the smaller axis would only reach 16 px and stop short).
    auto painted = [&](int x, int y) { return out.pixel(x, y).a > 0.01f; };
    EXPECT_TRUE(painted(32, 48 + 20)) << "vertical footprint must reach "
                                         "0.25 UV (~24px), not 0.25*width";
    EXPECT_TRUE(painted(32, 48 - 20));
    EXPECT_FALSE(painted(32, 48 + 30)) << "and must stop at the UV radius";
    // Horizontal radius is 0.25 * 64 = 16 px.
    EXPECT_TRUE(painted(32 + 13, 48));
    EXPECT_FALSE(painted(32 + 20, 48));
}
