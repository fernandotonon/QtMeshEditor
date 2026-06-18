// Pure-data unit tests for the PbrMapSynth building blocks (#404). These cover
// the tensor packing, normal/height decoding, and the roughness heuristic —
// none of which need ONNX Runtime or a GL context, so they run on any build
// (ENABLE_ONNX on or off) under Xvfb in CI.

#include <gtest/gtest.h>

#include <QColor>
#include <QImage>
#include <vector>

#include "PbrMapSynth.h"

using namespace PbrMapSynth;

namespace {

QImage solid(int w, int h, int r, int g, int b)
{
    QImage img(w, h, QImage::Format_RGB888);
    img.fill(qRgb(r, g, b));
    return img;
}

} // namespace

// RGB → NCHW planar float, normalized to [0,1], correct channel order.
TEST(PbrMapSynthCore, ToNchwRgbPlanarNormalized)
{
    QImage img = solid(2, 2, 255, 128, 0);
    std::vector<float> t = toNCHW(img, 3);
    ASSERT_EQ(t.size(), static_cast<size_t>(3 * 2 * 2));
    const size_t plane = 4;
    // R plane ~1.0, G plane ~0.5, B plane 0.0
    for (size_t i = 0; i < plane; ++i) {
        EXPECT_NEAR(t[0 * plane + i], 1.0f, 1e-3f);
        EXPECT_NEAR(t[1 * plane + i], 128.0f / 255.0f, 1e-3f);
        EXPECT_NEAR(t[2 * plane + i], 0.0f, 1e-3f);
    }
}

// Single-channel pack uses Rec.601 luminance.
TEST(PbrMapSynthCore, ToNchwGrayscaleLuma)
{
    QImage img = solid(1, 1, 255, 255, 255);
    std::vector<float> t = toNCHW(img, 1);
    ASSERT_EQ(t.size(), 1u);
    EXPECT_NEAR(t[0], 1.0f, 1e-3f);

    QImage black = solid(1, 1, 0, 0, 0);
    EXPECT_NEAR(toNCHW(black, 1)[0], 0.0f, 1e-3f);
}

// nchwToRgb is the inverse of toNCHW within rounding tolerance.
TEST(PbrMapSynthCore, NchwRoundTrip)
{
    QImage src = solid(3, 2, 200, 100, 50);
    std::vector<float> t = toNCHW(src, 3);
    QImage back = nchwToRgb(t, 3, 2);
    ASSERT_EQ(back.size(), src.size());
    for (int y = 0; y < 2; ++y)
        for (int x = 0; x < 3; ++x) {
            const QColor a = src.pixelColor(x, y), b = back.pixelColor(x, y);
            EXPECT_LE(std::abs(a.red() - b.red()), 1);
            EXPECT_LE(std::abs(a.green() - b.green()), 1);
            EXPECT_LE(std::abs(a.blue() - b.blue()), 1);
        }
}

// A flat surface (normal pointing straight up, encoded 0.5,0.5,1.0) decodes to
// the canonical flat-blue normal (~128,128,255).
TEST(PbrMapSynthCore, DecodeNormalFlatIsBlue)
{
    const int w = 2, h = 2;
    std::vector<float> t(static_cast<size_t>(3) * w * h);
    const size_t plane = static_cast<size_t>(w) * h;
    for (size_t i = 0; i < plane; ++i) {
        t[0 * plane + i] = 0.5f; // x = 0
        t[1 * plane + i] = 0.5f; // y = 0
        t[2 * plane + i] = 1.0f; // z = +1
    }
    QImage n = decodeNormal(t, w, h, 1.0f, /*invertG=*/false);
    const QColor c = n.pixelColor(0, 0);
    EXPECT_NEAR(c.red(), 128, 2);
    EXPECT_NEAR(c.green(), 128, 2);
    EXPECT_GE(c.blue(), 250);
}

// invertG flips the green channel about the midpoint (OpenGL ↔ DirectX).
TEST(PbrMapSynthCore, DecodeNormalInvertGFlipsGreen)
{
    const int w = 1, h = 1;
    std::vector<float> t(3);
    t[0] = 0.5f; t[1] = 0.9f; t[2] = 1.0f; // strong +Y tilt
    const int gOgl = decodeNormal(t, w, h, 1.0f, false).pixelColor(0, 0).green();
    const int gDx  = decodeNormal(t, w, h, 1.0f, true ).pixelColor(0, 0).green();
    // One should be above the midpoint, the other below.
    EXPECT_TRUE((gOgl > 128 && gDx < 128) || (gOgl < 128 && gDx > 128))
        << "gOgl=" << gOgl << " gDx=" << gDx;
}

// Height tensor scales linearly into a grayscale image.
TEST(PbrMapSynthCore, DecodeHeightScales)
{
    const int w = 2, h = 1;
    std::vector<float> t = {0.0f, 1.0f};
    QImage img = decodeHeight(t, w, h);
    ASSERT_EQ(img.format(), QImage::Format_Grayscale8);
    EXPECT_LE(qGray(img.pixel(0, 0)), 1);
    EXPECT_GE(qGray(img.pixel(1, 0)), 254);
}

// Roughness heuristic: darker albedo → rougher, brighter → smoother.
TEST(PbrMapSynthCore, RoughnessFromAlbedoDarkVsBright)
{
    QImage dark = solid(8, 8, 10, 10, 10);
    QImage bright = solid(8, 8, 245, 245, 245);
    QImage rDark = roughnessFromAlbedo(dark, 0.5f, 0.5f);
    QImage rBright = roughnessFromAlbedo(bright, 0.5f, 0.5f);
    ASSERT_FALSE(rDark.isNull());
    ASSERT_FALSE(rBright.isNull());
    EXPECT_GT(qGray(rDark.pixel(4, 4)), qGray(rBright.pixel(4, 4)));
}

// A flat-luminance albedo yields a (near-)flat roughness at the expected level.
TEST(PbrMapSynthCore, RoughnessFlatLuminanceIsUniform)
{
    QImage mid = solid(16, 16, 128, 128, 128);
    QImage r = roughnessFromAlbedo(mid, 0.5f, 0.5f);
    const int center = qGray(r.pixel(8, 8));
    // base 0.5 + contrast 0.5*(1 - ~0.5) ≈ 0.75 → ~191
    EXPECT_NEAR(center, 191, 12);
    // Interior should be uniform (blurred flat input).
    EXPECT_NEAR(qGray(r.pixel(4, 4)), center, 4);
}

// Without ENABLE_ONNX, synthesize() reports the not-built error cleanly.
#ifndef ENABLE_ONNX
TEST(PbrMapSynthCore, SynthesizeWithoutOnnxFailsCleanly)
{
    QImage albedo = solid(8, 8, 100, 120, 140);
    Result r = synthesize(albedo, "/nonexistent/model.onnx", {});
    EXPECT_FALSE(r.ok);
    EXPECT_FALSE(r.error.isEmpty());
}
#endif
