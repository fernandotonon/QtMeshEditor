#include <gtest/gtest.h>

#include "MultiViewTextureBaker.h"
#include "TexturePaintBuffer.h"

#include <QImage>

#include <OgreMatrix4.h>
#include <OgreVector2.h>
#include <OgreVector3.h>

#include <cmath>

// Pure-data tests for the multi-view projection bake (slice 1). No Ogre scene
// manager, no GL, no QApplication, no SD — the baker takes plain geometry +
// QImages + matrices, so the reprojection math is fully verifiable headlessly.

namespace {

// Build view*proj for a camera at `eye` looking toward `target` with the given
// up, perspective FOVy (radians), aspect 1, near/far. Mirrors what
// MeshDepthRenderer captures (Ogre::Camera::getViewMatrix/getProjectionMatrix).
Ogre::Matrix4 makeViewProj(const Ogre::Vector3& eye, const Ogre::Vector3& target,
                           const Ogre::Vector3& up, float fovY, float aspect,
                           float nearC, float farC)
{
    // View matrix (right-handed look-at, world -> camera).
    Ogre::Vector3 zAxis = (eye - target);   // camera looks down -Z
    zAxis.normalise();
    Ogre::Vector3 xAxis = up.crossProduct(zAxis); xAxis.normalise();
    Ogre::Vector3 yAxis = zAxis.crossProduct(xAxis);
    Ogre::Matrix4 view = Ogre::Matrix4::IDENTITY;
    view[0][0] = xAxis.x; view[0][1] = xAxis.y; view[0][2] = xAxis.z; view[0][3] = -xAxis.dotProduct(eye);
    view[1][0] = yAxis.x; view[1][1] = yAxis.y; view[1][2] = yAxis.z; view[1][3] = -yAxis.dotProduct(eye);
    view[2][0] = zAxis.x; view[2][1] = zAxis.y; view[2][2] = zAxis.z; view[2][3] = -zAxis.dotProduct(eye);

    // Perspective projection (OpenGL-style, clip z in [-1,1]).
    const float f = 1.0f / std::tan(fovY * 0.5f);
    Ogre::Matrix4 proj = Ogre::Matrix4::ZERO;
    proj[0][0] = f / aspect;
    proj[1][1] = f;
    proj[2][2] = (farC + nearC) / (nearC - farC);
    proj[2][3] = (2.0f * farC * nearC) / (nearC - farC);
    proj[3][2] = -1.0f;
    return proj * view;
}

// A solid-colour image.
QImage solid(int size, int r, int g, int b)
{
    QImage img(size, size, QImage::Format_RGB888);
    img.fill(qRgb(r, g, b));
    return img;
}

// Two triangles forming a quad in the Z=0 plane spanning [-1,1] in X/Y, with
// UV0 covering the full [0..1] atlas. `frontFacing` chooses the winding/normal:
// +Z normal (faces a +Z camera) or -Z normal.
std::vector<MultiViewTextureBaker::Triangle> makeQuad(const Ogre::Vector3& normal)
{
    const Ogre::Vector3 bl(-1, -1, 0), br(1, -1, 0), tr(1, 1, 0), tl(-1, 1, 0);
    // UVs: bl=(0,1) br=(1,1) tr=(1,0) tl=(0,0) (top-left origin, V down).
    const Ogre::Vector2 ubl(0, 1), ubr(1, 1), utr(1, 0), utl(0, 0);
    MultiViewTextureBaker::Triangle t0, t1;
    t0.p[0] = bl; t0.p[1] = br; t0.p[2] = tr;
    t0.uv[0] = ubl; t0.uv[1] = ubr; t0.uv[2] = utr; t0.normal = normal;
    t1.p[0] = bl; t1.p[1] = tr; t1.p[2] = tl;
    t1.uv[0] = ubl; t1.uv[1] = utr; t1.uv[2] = utl; t1.normal = normal;
    return { t0, t1 };
}

MultiViewTextureBaker::View frontView(const QImage& img)
{
    MultiViewTextureBaker::View v;
    v.image = img;
    // Camera on +Z looking toward -Z (origin). camDirection points INTO scene.
    v.viewProj = makeViewProj(Ogre::Vector3(0, 0, 4), Ogre::Vector3(0, 0, 0),
                              Ogre::Vector3::UNIT_Y, 1.2f, 1.0f, 0.1f, 100.0f);
    v.camDirection = Ogre::Vector3(0, 0, -1);
    return v;
}

MultiViewTextureBaker::View backView(const QImage& img)
{
    MultiViewTextureBaker::View v;
    v.image = img;
    v.viewProj = makeViewProj(Ogre::Vector3(0, 0, -4), Ogre::Vector3(0, 0, 0),
                              Ogre::Vector3::UNIT_Y, 1.2f, 1.0f, 0.1f, 100.0f);
    v.camDirection = Ogre::Vector3(0, 0, 1);
    return v;
}

// Read the bake's colour at a UV (top-left origin).
Ogre::ColourValue at(const TexturePaintBuffer& buf, float u, float v)
{
    int x = std::clamp(static_cast<int>(u * buf.width()), 0, buf.width() - 1);
    int y = std::clamp(static_cast<int>(v * buf.height()), 0, buf.height() - 1);
    return buf.pixel(x, y);
}

} // namespace

TEST(MultiViewTextureBakerTest, EmptyInputsAreRejected)
{
    TexturePaintBuffer out;
    MultiViewTextureBaker::Options opts;
    EXPECT_FALSE(MultiViewTextureBaker::bake({}, { frontView(solid(8, 255, 0, 0)) }, out, opts).ok);
    EXPECT_FALSE(MultiViewTextureBaker::bake(makeQuad(Ogre::Vector3::UNIT_Z), {}, out, opts).ok);
    // A view with a null image is rejected.
    MultiViewTextureBaker::View bad; bad.camDirection = Ogre::Vector3(0,0,-1);
    EXPECT_FALSE(MultiViewTextureBaker::bake(makeQuad(Ogre::Vector3::UNIT_Z), { bad }, out, opts).ok);
}

TEST(MultiViewTextureBakerTest, FrontViewPaintsFrontFacingQuad)
{
    // A quad whose normal faces +Z, lit only by the +Z (front) camera with a
    // red image, should come out red across the whole atlas.
    auto tris = makeQuad(Ogre::Vector3::UNIT_Z);
    TexturePaintBuffer out;
    MultiViewTextureBaker::Options opts;
    opts.resolution = 64;
    opts.dilationPixels = 2;
    auto rep = MultiViewTextureBaker::bake(tris, { frontView(solid(16, 255, 0, 0)) }, out, opts);
    ASSERT_TRUE(rep.ok) << rep.error.toStdString();
    EXPECT_GT(rep.pixelsWritten, 0);
    EXPECT_EQ(rep.trianglesProjected, 2);  // both tris faced the front camera
    EXPECT_EQ(out.width(), 64);

    const Ogre::ColourValue c = at(out, 0.5f, 0.5f);  // centre of atlas
    EXPECT_NEAR(c.r, 1.0f, 0.02f);
    EXPECT_NEAR(c.g, 0.0f, 0.02f);
    EXPECT_NEAR(c.b, 0.0f, 0.02f);
    EXPECT_NEAR(c.a, 1.0f, 0.02f);
}

TEST(MultiViewTextureBakerTest, BackCameraSkipsFrontFacingQuad)
{
    // The same +Z-facing quad seen ONLY by the back (-Z) camera: facing weight
    // is negative, so nothing is painted (transparent background remains).
    auto tris = makeQuad(Ogre::Vector3::UNIT_Z);
    TexturePaintBuffer out;
    MultiViewTextureBaker::Options opts;
    opts.resolution = 64;
    opts.dilationPixels = 0;
    auto rep = MultiViewTextureBaker::bake(tris, { backView(solid(16, 0, 0, 255)) }, out, opts);
    ASSERT_TRUE(rep.ok) << rep.error.toStdString();
    EXPECT_EQ(rep.pixelsWritten, 0);
    EXPECT_EQ(rep.trianglesProjected, 0);
    EXPECT_NEAR(at(out, 0.5f, 0.5f).a, 0.0f, 0.02f);  // background, untouched
}

TEST(MultiViewTextureBakerTest, FrontAndBackBlendByFacing)
{
    // Front-facing quad with BOTH a red front view and a blue back view.
    // Only the front camera faces the +Z normal, so red must win regardless of
    // the blue back image being supplied.
    auto tris = makeQuad(Ogre::Vector3::UNIT_Z);
    TexturePaintBuffer out;
    MultiViewTextureBaker::Options opts;
    opts.resolution = 64;
    opts.dilationPixels = 2;
    std::vector<MultiViewTextureBaker::View> views = {
        frontView(solid(16, 255, 0, 0)),
        backView(solid(16, 0, 0, 255)),
    };
    auto rep = MultiViewTextureBaker::bake(tris, views, out, opts);
    ASSERT_TRUE(rep.ok) << rep.error.toStdString();
    ASSERT_EQ(rep.perViewTriangleCount.size(), 2u);
    EXPECT_EQ(rep.perViewTriangleCount[0], 2);  // front contributed
    EXPECT_EQ(rep.perViewTriangleCount[1], 0);  // back culled by facing

    const Ogre::ColourValue c = at(out, 0.5f, 0.5f);
    EXPECT_NEAR(c.r, 1.0f, 0.02f);
    EXPECT_NEAR(c.b, 0.0f, 0.02f);
}

TEST(MultiViewTextureBakerTest, DilationFillsBackgroundNeighbours)
{
    // With a sub-region quad (UVs in [0.25..0.75]) the atlas has a wide
    // background margin; dilation must flip some of those background texels.
    MultiViewTextureBaker::Triangle t0, t1;
    const Ogre::Vector3 bl(-1,-1,0), br(1,-1,0), tr(1,1,0), tl(-1,1,0);
    t0.p[0]=bl; t0.p[1]=br; t0.p[2]=tr;
    t0.uv[0]=Ogre::Vector2(0.25f,0.75f); t0.uv[1]=Ogre::Vector2(0.75f,0.75f); t0.uv[2]=Ogre::Vector2(0.75f,0.25f);
    t0.normal=Ogre::Vector3::UNIT_Z;
    t1.p[0]=bl; t1.p[1]=tr; t1.p[2]=tl;
    t1.uv[0]=Ogre::Vector2(0.25f,0.75f); t1.uv[1]=Ogre::Vector2(0.75f,0.25f); t1.uv[2]=Ogre::Vector2(0.25f,0.25f);
    t1.normal=Ogre::Vector3::UNIT_Z;

    TexturePaintBuffer out;
    MultiViewTextureBaker::Options opts;
    opts.resolution = 64;
    opts.dilationPixels = 3;
    auto rep = MultiViewTextureBaker::bake({t0,t1}, { frontView(solid(16, 0, 255, 0)) }, out, opts);
    ASSERT_TRUE(rep.ok) << rep.error.toStdString();
    EXPECT_GT(rep.pixelsWritten, 0);
    EXPECT_GT(rep.pixelsDilated, 0);  // some background texels got smeared
    // Centre is inside the island → green.
    EXPECT_NEAR(at(out, 0.5f, 0.5f).g, 1.0f, 0.02f);
}

TEST(MultiViewTextureBakerTest, ColorMatchShiftsLaterViewTowardFirst)
{
    // Two cameras that BOTH face the +Z quad (front at +Z, and a second one
    // slightly off +Z so its facing weight is also positive). The first view is
    // mid-grey; the second is dark. With colorMatchToFirstView on, the second
    // image's mean is lifted toward the first, so the blended result is brighter
    // than it would be with the dark second image left as-is.
    auto tris = makeQuad(Ogre::Vector3::UNIT_Z);

    MultiViewTextureBaker::View v0 = frontView(solid(16, 130, 130, 130)); // grey
    MultiViewTextureBaker::View v1;                                        // dark
    v1.image = solid(16, 20, 20, 20);
    v1.viewProj = makeViewProj(Ogre::Vector3(0.6f, 0, 4), Ogre::Vector3(0, 0, 0),
                               Ogre::Vector3::UNIT_Y, 1.2f, 1.0f, 0.1f, 100.0f);
    v1.camDirection = Ogre::Vector3(-0.15f, 0, -1).normalisedCopy();

    auto bakeWith = [&](bool match) {
        TexturePaintBuffer out;
        MultiViewTextureBaker::Options opts;
        opts.resolution = 32;
        opts.dilationPixels = 0;
        opts.colorMatchToFirstView = match;
        auto rep = MultiViewTextureBaker::bake(tris, { v0, v1 }, out, opts);
        EXPECT_TRUE(rep.ok) << rep.error.toStdString();
        return at(out, 0.5f, 0.5f).r;
    };

    const float matched = bakeWith(true);
    const float unmatched = bakeWith(false);
    // Matching lifts the dark second view toward the grey first view, so the
    // blended centre is brighter than without matching.
    EXPECT_GT(matched, unmatched);
}
