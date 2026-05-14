#include <gtest/gtest.h>

#include "EditableMesh.h"
#include "TexturePaintBuffer.h"
#include "VertexColorBaker.h"

namespace {

EditableMesh makeUnitTriangleMesh(const Ogre::ColourValue& c0,
                                  const Ogre::ColourValue& c1,
                                  const Ogre::ColourValue& c2)
{
    EditableMesh mesh;
    EditableSubMesh sub;
    EditableVertex v0; v0.position = {0, 0, 0}; v0.uv = {0.0f, 0.0f}; v0.color = c0;
    v0.hasUV = true; v0.hasColor = true;
    EditableVertex v1; v1.position = {1, 0, 0}; v1.uv = {1.0f, 0.0f}; v1.color = c1;
    v1.hasUV = true; v1.hasColor = true;
    EditableVertex v2; v2.position = {0, 1, 0}; v2.uv = {0.0f, 1.0f}; v2.color = c2;
    v2.hasUV = true; v2.hasColor = true;
    sub.vertices = { v0, v1, v2 };
    EditableTriangle tri; tri.indices[0] = 0; tri.indices[1] = 1; tri.indices[2] = 2;
    sub.triangles = { tri };
    mesh.subMeshes() = { sub };
    return mesh;
}

} // namespace

TEST(VertexColorBakerTest, RasterizeFlatTriangleSolidColor)
{
    TexturePaintBuffer buf(64, 64);
    buf.clear(Ogre::ColourValue(0, 0, 0, 0)); // transparent black bg
    const int painted = VertexColorBaker::rasterizeTriangle(
        buf,
        Ogre::Vector2(0.0f, 0.0f),
        Ogre::Vector2(1.0f, 0.0f),
        Ogre::Vector2(0.0f, 1.0f),
        Ogre::ColourValue::Red, Ogre::ColourValue::Red, Ogre::ColourValue::Red);
    EXPECT_GT(painted, 1000); // ~half of 64*64 ~= 2048
    // Pixel near vertex 0 (uv 0,0 → top-left pixel (0,0)) should be red.
    const auto p0 = buf.pixel(2, 2);
    EXPECT_NEAR(p0.r, 1.0f, 0.05f);
    EXPECT_NEAR(p0.g, 0.0f, 0.05f);
    // Pixel inside the triangle, near center.
    const auto pc = buf.pixel(10, 10);
    EXPECT_NEAR(pc.r, 1.0f, 0.05f);
}

TEST(VertexColorBakerTest, RasterizeBarycentricInterpolatesColors)
{
    TexturePaintBuffer buf(128, 128);
    buf.clear(Ogre::ColourValue(0, 0, 0, 0));
    VertexColorBaker::rasterizeTriangle(
        buf,
        Ogre::Vector2(0.0f, 0.0f),
        Ogre::Vector2(1.0f, 0.0f),
        Ogre::Vector2(0.0f, 1.0f),
        Ogre::ColourValue(1.0f, 0.0f, 0.0f, 1.0f),  // red at (0,0)
        Ogre::ColourValue(0.0f, 1.0f, 0.0f, 1.0f),  // green at (1,0)
        Ogre::ColourValue(0.0f, 0.0f, 1.0f, 1.0f)); // blue at (0,1)

    // Pixel near v0 (uv 0,0 → pixel (0,0) since UV origin is top-left).
    // Use small offsets to stay inside the triangle.
    int x0=0, y0=0; buf.uvToPixel(Ogre::Vector2(0.02f, 0.02f), x0, y0);
    const auto cNearV0 = buf.pixel(x0, y0);
    EXPECT_GT(cNearV0.r, 0.85f);
    EXPECT_LT(cNearV0.g, 0.15f);
    EXPECT_LT(cNearV0.b, 0.15f);

    // Pixel near v1 (uv 1,0).
    int x1=0, y1=0; buf.uvToPixel(Ogre::Vector2(0.95f, 0.02f), x1, y1);
    const auto cNearV1 = buf.pixel(x1, y1);
    EXPECT_LT(cNearV1.r, 0.15f);
    EXPECT_GT(cNearV1.g, 0.80f);
    EXPECT_LT(cNearV1.b, 0.15f);

    // Pixel near v2 (uv 0,1).
    int x2=0, y2=0; buf.uvToPixel(Ogre::Vector2(0.02f, 0.95f), x2, y2);
    const auto cNearV2 = buf.pixel(x2, y2);
    EXPECT_LT(cNearV2.r, 0.15f);
    EXPECT_LT(cNearV2.g, 0.15f);
    EXPECT_GT(cNearV2.b, 0.80f);
}

TEST(VertexColorBakerTest, RasterizeDegenerateTriangleIsNoop)
{
    TexturePaintBuffer buf(32, 32);
    buf.clear(Ogre::ColourValue(0, 0, 0, 0));
    // Collinear points (uv-space line, not a triangle).
    const int painted = VertexColorBaker::rasterizeTriangle(
        buf,
        Ogre::Vector2(0.0f, 0.0f),
        Ogre::Vector2(0.5f, 0.5f),
        Ogre::Vector2(1.0f, 1.0f),
        Ogre::ColourValue::Red,
        Ogre::ColourValue::Green,
        Ogre::ColourValue::Blue);
    EXPECT_EQ(painted, 0);
}

TEST(VertexColorBakerTest, RasterizeFlippedWindingStillCoversTriangle)
{
    TexturePaintBuffer buf(64, 64);
    buf.clear(Ogre::ColourValue(0, 0, 0, 0));
    // CCW vs CW: the rasterizer should handle both consistently.
    const int paintedCcw = VertexColorBaker::rasterizeTriangle(
        buf,
        Ogre::Vector2(0.0f, 0.0f),
        Ogre::Vector2(1.0f, 0.0f),
        Ogre::Vector2(0.0f, 1.0f),
        Ogre::ColourValue::Red, Ogre::ColourValue::Red, Ogre::ColourValue::Red);
    EXPECT_GT(paintedCcw, 100);

    TexturePaintBuffer buf2(64, 64);
    buf2.clear(Ogre::ColourValue(0, 0, 0, 0));
    // Same triangle, swapped winding.
    const int paintedCw = VertexColorBaker::rasterizeTriangle(
        buf2,
        Ogre::Vector2(0.0f, 0.0f),
        Ogre::Vector2(0.0f, 1.0f),
        Ogre::Vector2(1.0f, 0.0f),
        Ogre::ColourValue::Red, Ogre::ColourValue::Red, Ogre::ColourValue::Red);
    // Should cover the same number of pixels (within rounding).
    EXPECT_NEAR(paintedCw, paintedCcw, paintedCcw / 10);
}

TEST(VertexColorBakerTest, BakeProducesNonEmptyOutput)
{
    EditableMesh mesh = makeUnitTriangleMesh(
        Ogre::ColourValue::Red,
        Ogre::ColourValue::Green,
        Ogre::ColourValue::Blue);
    TexturePaintBuffer buf;
    VertexColorBaker::Options opts;
    opts.resolution = 128;
    opts.dilationPixels = 0;
    const int painted = VertexColorBaker::bake(mesh, buf, opts);
    EXPECT_GT(painted, 1000);
    EXPECT_EQ(buf.width(), 128);
    EXPECT_EQ(buf.height(), 128);
}

TEST(VertexColorBakerTest, BakeDefaultsToWhiteBackground)
{
    EditableMesh mesh; // empty mesh
    TexturePaintBuffer buf;
    VertexColorBaker::Options opts;
    opts.resolution = 64;
    opts.background = Ogre::ColourValue(0.25f, 0.25f, 0.25f, 1.0f);
    const int painted = VertexColorBaker::bake(mesh, buf, opts);
    EXPECT_EQ(painted, 0);
    EXPECT_EQ(buf.width(), 64);
    // All pixels should be the background color.
    const auto p = buf.pixel(0, 0);
    EXPECT_NEAR(p.r, 0.25f, 0.02f);
    EXPECT_NEAR(p.g, 0.25f, 0.02f);
}

TEST(VertexColorBakerTest, DilationExpandsRasterizedRegion)
{
    EditableMesh mesh = makeUnitTriangleMesh(
        Ogre::ColourValue::Red,
        Ogre::ColourValue::Red,
        Ogre::ColourValue::Red);

    TexturePaintBuffer bufNoDilate;
    VertexColorBaker::Options optsNo;
    optsNo.resolution = 128;
    optsNo.dilationPixels = 0;
    optsNo.background = Ogre::ColourValue(0, 0, 0, 0);
    const int paintedNo = VertexColorBaker::bake(mesh, bufNoDilate, optsNo);

    TexturePaintBuffer bufDilate;
    VertexColorBaker::Options optsYes = optsNo;
    optsYes.dilationPixels = 6;
    const int paintedYes = VertexColorBaker::bake(mesh, bufDilate, optsYes);

    // Count red pixels in each.
    auto countRed = [](const TexturePaintBuffer& b) {
        int n = 0;
        for (int y = 0; y < b.height(); ++y)
            for (int x = 0; x < b.width(); ++x)
                if (b.pixel(x, y).r > 0.5f) ++n;
        return n;
    };
    const int nNo = countRed(bufNoDilate);
    const int nYes = countRed(bufDilate);
    EXPECT_GT(nYes, nNo) << "Dilation must expand the rasterized region";
    EXPECT_EQ(paintedNo, paintedYes) << "Rasterized count (before dilation) should match";
}

TEST(VertexColorBakerTest, BakeConvenienceOverloadUsesDefaults)
{
    EditableMesh mesh = makeUnitTriangleMesh(
        Ogre::ColourValue::White,
        Ogre::ColourValue::White,
        Ogre::ColourValue::White);
    TexturePaintBuffer buf;
    const int painted = VertexColorBaker::bake(mesh, buf);
    EXPECT_GT(painted, 100000); // 1024x1024 default has ~half-million rasterized pixels for this tri
    EXPECT_EQ(buf.width(), 1024);
}
