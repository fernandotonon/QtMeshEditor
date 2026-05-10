#include <gtest/gtest.h>

#include <QImage>
#include <QImageReader>
#include <QTemporaryDir>

#include "TextureChannelPacker.h"

using namespace TextureChannelPacker;

namespace {

// Write a constant-grey N×M PNG to disk so the packer has a real source.
QString writeGreyPng(const QTemporaryDir& dir,
                     const QString& name,
                     int w, int h, int grey)
{
    QImage img(w, h, QImage::Format_RGBA8888);
    img.fill(qRgba(grey, grey, grey, 255));
    const QString path = dir.filePath(name);
    [&]() { ASSERT_TRUE(img.save(path, "PNG")) << path.toStdString(); }();
    return path;
}

uint8_t pixel(const QImage& img, int x, int y, int channel)
{
    QRgb p = img.pixel(x, y);
    switch (channel) {
        case 0: return qRed(p);
        case 1: return qGreen(p);
        case 2: return qBlue(p);
        case 3: return qAlpha(p);
    }
    return 0;
}

} // namespace

TEST(TextureChannelPackerTest, AllConstantsProducesSolidColour) {
    PackingSpec spec;
    spec.red.constantValue = 1.0f;     // 255
    spec.green.constantValue = 0.5f;   // ~128
    spec.blue.constantValue = 0.0f;    // 0
    spec.alpha.constantValue = 1.0f;   // 255

    PackResult r = pack(spec);
    ASSERT_TRUE(r.ok) << r.error.toStdString();
    ASSERT_FALSE(r.image.isNull());
    EXPECT_EQ(r.usedWidth, 256);   // default size when no sources
    EXPECT_EQ(r.usedHeight, 256);

    // Spot-check a pixel.
    EXPECT_EQ(pixel(r.image, 10, 10, 0), 255);
    EXPECT_NEAR(pixel(r.image, 10, 10, 1), 128, 1);
    EXPECT_EQ(pixel(r.image, 10, 10, 2), 0);
    EXPECT_EQ(pixel(r.image, 10, 10, 3), 255);
}

TEST(TextureChannelPackerTest, ORM_PacksThreeGreyImagesIntoRGB) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());

    // Simulate Unity ORM: AO=64 → R, Roughness=200 → G, Metallic=10 → B.
    const QString aoPath    = writeGreyPng(tmp, "ao.png",        32, 32,  64);
    const QString roughPath = writeGreyPng(tmp, "roughness.png", 32, 32, 200);
    const QString metalPath = writeGreyPng(tmp, "metallic.png",  32, 32,  10);

    PackingSpec spec;
    spec.red.path   = aoPath;
    spec.green.path = roughPath;
    spec.blue.path  = metalPath;
    spec.alpha.constantValue = 1.0f;

    PackResult r = pack(spec);
    ASSERT_TRUE(r.ok) << r.error.toStdString();
    EXPECT_EQ(r.usedWidth, 32);
    EXPECT_EQ(r.usedHeight, 32);

    // Every pixel of the output should match the source greys (Rec.601
    // luminance of a fully-grey pixel is the grey value itself).
    EXPECT_EQ(pixel(r.image, 0, 0, 0),  64);
    EXPECT_EQ(pixel(r.image, 0, 0, 1), 200);
    EXPECT_EQ(pixel(r.image, 0, 0, 2),  10);
    EXPECT_EQ(pixel(r.image, 0, 0, 3), 255);
}

TEST(TextureChannelPackerTest, InvertFlagFlipsChannel) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString src = writeGreyPng(tmp, "rough.png", 8, 8, 200);

    // Roughness=200 inverted → Glossiness=55. Common Specular-Glossiness
    // workflow conversion.
    PackingSpec spec;
    spec.red.path = src;
    spec.red.invert = true;
    PackResult r = pack(spec);
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(pixel(r.image, 4, 4, 0), 55);
}

TEST(TextureChannelPackerTest, MismatchedSizesAreScaledToLargest) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString small = writeGreyPng(tmp, "small.png", 16, 16, 100);
    const QString big   = writeGreyPng(tmp, "big.png",   64, 64, 200);

    PackingSpec spec;
    spec.red.path   = small;
    spec.green.path = big;
    PackResult r = pack(spec);
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(r.usedWidth, 64);
    EXPECT_EQ(r.usedHeight, 64);
    // Centre pixel should still be ~100 (small was scaled up uniformly).
    EXPECT_NEAR(pixel(r.image, 32, 32, 0), 100, 2);
    EXPECT_NEAR(pixel(r.image, 32, 32, 1), 200, 2);
}

TEST(TextureChannelPackerTest, MissingFileReturnsError) {
    PackingSpec spec;
    spec.red.path = "/nonexistent/path/does_not_exist.png";

    PackResult r = pack(spec);
    EXPECT_FALSE(r.ok);
    EXPECT_FALSE(r.error.isEmpty());
}

TEST(TextureChannelPackerTest, ExplicitOutputSizeIsRespected) {
    PackingSpec spec;
    spec.red.constantValue = 0.5f;
    spec.outputWidth  = 128;
    spec.outputHeight = 64;
    PackResult r = pack(spec);
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(r.usedWidth, 128);
    EXPECT_EQ(r.usedHeight, 64);
    EXPECT_EQ(r.image.width(), 128);
    EXPECT_EQ(r.image.height(), 64);
}

TEST(TextureChannelPackerTest, IncludeAlphaFalseProducesRGB888) {
    PackingSpec spec;
    spec.red.constantValue = 1.0f;
    spec.includeAlpha = false;
    PackResult r = pack(spec);
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(r.image.format(), QImage::Format_RGB888);
}

TEST(TextureChannelPackerTest, PackToFileWritesPng) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString src = writeGreyPng(tmp, "src.png", 8, 8, 128);

    PackingSpec spec;
    spec.red.path = src;
    spec.green.path = src;
    spec.blue.path = src;
    const QString outPath = tmp.filePath("packed.png");
    PackResult r = packToFile(spec, outPath);
    ASSERT_TRUE(r.ok) << r.error.toStdString();

    // Re-read and verify the saved file actually contains the packed data.
    QImage reloaded(outPath);
    ASSERT_FALSE(reloaded.isNull());
    EXPECT_EQ(reloaded.width(), 8);
    EXPECT_EQ(reloaded.height(), 8);
    QRgb px = reloaded.pixel(4, 4);
    EXPECT_EQ(qRed(px),   128);
    EXPECT_EQ(qGreen(px), 128);
    EXPECT_EQ(qBlue(px),  128);
}

TEST(TextureChannelPackerTest, PackToFile_EmptyPathFails) {
    PackingSpec spec;
    spec.red.constantValue = 0.5f;
    PackResult r = packToFile(spec, "");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(r.error.contains("output path", Qt::CaseInsensitive));
}

TEST(TextureChannelPackerTest, PackToFile_UnsupportedExtensionFails) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    PackingSpec spec;
    spec.red.constantValue = 0.5f;
    PackResult r = packToFile(spec, tmp.filePath("nope.weirdext"));
    EXPECT_FALSE(r.ok);
}
