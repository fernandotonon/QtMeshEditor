#include <gtest/gtest.h>

#include <QImage>
#include <QTemporaryDir>

#include "NormalMapGenerator.h"

using namespace NormalMapGenerator;

namespace {

// Write an N×M PNG with `gradientFn(x, y)` returning a 0..255 grey
// value per pixel. Returns the on-disk path.
template <class F>
QString writePng(const QTemporaryDir& dir, const QString& name,
                 int w, int h, F gradientFn)
{
    QImage img(w, h, QImage::Format_RGBA8888);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const int v = gradientFn(x, y);
            img.setPixel(x, y, qRgba(v, v, v, 255));
        }
    }
    const QString path = dir.filePath(name);
    [&]() { ASSERT_TRUE(img.save(path, "PNG")) << path.toStdString(); }();
    return path;
}

} // namespace

TEST(NormalMapGeneratorTest, FlatHeightProducesStraightUpNormals) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString src = writePng(tmp, "flat.png", 16, 16,
                                  [](int, int){ return 128; });

    GenSpec spec;
    spec.sourcePath = src;
    spec.strength = 2.0f;
    GenResult r = generate(spec);
    ASSERT_TRUE(r.ok) << r.error.toStdString();
    EXPECT_EQ(r.usedWidth, 16);
    EXPECT_EQ(r.usedHeight, 16);

    // Flat heightmap → gradient zero → normal = (0, 0, 1) → encoded
    // as (~128, ~128, 255). Sample the centre pixel where edge effects
    // are absent.
    const QRgb px = r.image.pixel(8, 8);
    EXPECT_NEAR(qRed(px),   128, 2);
    EXPECT_NEAR(qGreen(px), 128, 2);
    EXPECT_EQ(qBlue(px),    255);
}

TEST(NormalMapGeneratorTest, HorizontalGradientTiltsRedChannel) {
    // x increases left-to-right → ∂h/∂x positive → nx = -dx*strength
    // is negative → red channel encodes to less than 128.
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString src = writePng(tmp, "ramp_x.png", 16, 16,
                                  [](int x, int){ return std::min(255, x * 16); });

    GenSpec spec;
    spec.sourcePath = src;
    spec.strength = 2.0f;
    GenResult r = generate(spec);
    ASSERT_TRUE(r.ok);

    const QRgb px = r.image.pixel(8, 8);
    EXPECT_LT(qRed(px), 120);   // tilted toward -X
    EXPECT_NEAR(qGreen(px), 128, 4);  // no Y gradient
}

TEST(NormalMapGeneratorTest, VerticalGradientTiltsGreenChannel) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    // y increases top-to-bottom → ∂h/∂y positive → ny negative →
    // green encodes to less than 128.
    const QString src = writePng(tmp, "ramp_y.png", 16, 16,
                                  [](int, int y){ return std::min(255, y * 16); });

    GenSpec spec;
    spec.sourcePath = src;
    spec.strength = 2.0f;
    GenResult r = generate(spec);
    ASSERT_TRUE(r.ok);

    const QRgb px = r.image.pixel(8, 8);
    EXPECT_NEAR(qRed(px), 128, 4);
    EXPECT_LT(qGreen(px), 120);
}

TEST(NormalMapGeneratorTest, InvertGFlipsGreenChannel) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString src = writePng(tmp, "ramp_y.png", 16, 16,
                                  [](int, int y){ return std::min(255, y * 16); });

    GenSpec spec;
    spec.sourcePath = src;
    spec.strength = 2.0f;
    spec.invertG = true;
    GenResult r = generate(spec);
    ASSERT_TRUE(r.ok);

    // Was <120 without invert; should now be >135.
    const QRgb px = r.image.pixel(8, 8);
    EXPECT_GT(qGreen(px), 135);
}

TEST(NormalMapGeneratorTest, InvertRFlipsRedChannel) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString src = writePng(tmp, "ramp_x.png", 16, 16,
                                  [](int x, int){ return std::min(255, x * 16); });

    GenSpec spec;
    spec.sourcePath = src;
    spec.strength = 2.0f;
    spec.invertR = true;
    GenResult r = generate(spec);
    ASSERT_TRUE(r.ok);

    const QRgb px = r.image.pixel(8, 8);
    EXPECT_GT(qRed(px), 135);
}

TEST(NormalMapGeneratorTest, ZeroStrengthGivesFlatNormals) {
    // Even with a steep gradient, strength=0 should collapse the
    // in-plane components to zero → straight-up normal everywhere.
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString src = writePng(tmp, "ramp_x.png", 8, 8,
                                  [](int x, int){ return x * 32; });

    GenSpec spec;
    spec.sourcePath = src;
    spec.strength = 0.0f;
    GenResult r = generate(spec);
    ASSERT_TRUE(r.ok);

    const QRgb px = r.image.pixel(4, 4);
    EXPECT_EQ(qRed(px),   128);
    EXPECT_EQ(qGreen(px), 128);
    EXPECT_EQ(qBlue(px),  255);
}

TEST(NormalMapGeneratorTest, MissingSourceReturnsError) {
    GenSpec spec;
    spec.sourcePath = "/nonexistent/should_not_resolve.png";
    GenResult r = generate(spec);
    EXPECT_FALSE(r.ok);
    EXPECT_FALSE(r.error.isEmpty());
}

TEST(NormalMapGeneratorTest, EmptySourcePathReturnsError) {
    GenSpec spec;
    GenResult r = generate(spec);
    EXPECT_FALSE(r.ok);
}

TEST(NormalMapGeneratorTest, OutputDimensionsOverrideSource) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString src = writePng(tmp, "flat.png", 32, 32,
                                  [](int, int){ return 128; });

    GenSpec spec;
    spec.sourcePath = src;
    spec.outputWidth = 64;
    spec.outputHeight = 64;
    GenResult r = generate(spec);
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(r.usedWidth, 64);
    EXPECT_EQ(r.usedHeight, 64);
    EXPECT_EQ(r.image.width(), 64);
    EXPECT_EQ(r.image.height(), 64);
}

TEST(NormalMapGeneratorTest, OutputFormatIsRgb888) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString src = writePng(tmp, "flat.png", 8, 8,
                                  [](int, int){ return 128; });

    GenSpec spec;
    spec.sourcePath = src;
    GenResult r = generate(spec);
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(r.image.format(), QImage::Format_RGB888);
}

TEST(NormalMapGeneratorTest, GenerateToFileWritesPng) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString src = writePng(tmp, "flat.png", 8, 8,
                                  [](int, int){ return 128; });
    const QString out = tmp.filePath("normal.png");

    GenSpec spec;
    spec.sourcePath = src;
    GenResult r = generateToFile(spec, out);
    ASSERT_TRUE(r.ok) << r.error.toStdString();

    QImage reloaded(out);
    ASSERT_FALSE(reloaded.isNull());
    EXPECT_EQ(reloaded.width(), 8);
    EXPECT_EQ(reloaded.height(), 8);
}

TEST(NormalMapGeneratorTest, GenerateToFile_EmptyOutputPathFails) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString src = writePng(tmp, "flat.png", 4, 4,
                                  [](int, int){ return 128; });
    GenSpec spec;
    spec.sourcePath = src;
    GenResult r = generateToFile(spec, QString());
    EXPECT_FALSE(r.ok);
}
