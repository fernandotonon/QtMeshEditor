#include <gtest/gtest.h>

#include "TextureInpaint.h"

#include <QImage>

#include <cmath>
#include <limits>
#include <vector>

// TextureInpaint's pre/post-processing is pure data (no Ogre, no GL, no ONNX
// session), so these run on every build. The inference itself needs the hosted
// model and is exercised manually; what is pinned here is the CONTRACT the
// model relies on. Two of these traps are silent — they produce a
// plausible-looking texture rather than an error — so they are exactly the kind
// that survive to a release unless a test holds them down.

TEST(TextureInpaint, ImageTensorIsScaledToUnitRangeCHW)
{
    // The graph takes the image in [0,1] but RETURNS 0..255. Feeding it the
    // raw 0..255 image saturates the output to near-white (measured mean
    // 255.0 vs a correct 156.0) without any error being raised.
    QImage img(8, 8, QImage::Format_RGB888);
    img.fill(QColor(255, 128, 0));

    const int t = TextureInpaint::kInputSize;
    const auto in = TextureInpaint::toImageTensor(img, t);
    ASSERT_EQ(in.size(), static_cast<size_t>(t) * t * 3);

    const size_t plane = static_cast<size_t>(t) * t;
    EXPECT_NEAR(in[0], 1.0f, 1e-4f);              // R
    EXPECT_NEAR(in[plane], 128.0f / 255.0f, 1e-4f);  // G
    EXPECT_NEAR(in[2 * plane], 0.0f, 1e-4f);      // B
    // Nothing may exceed 1.0 — the saturation failure mode.
    for (float v : in) EXPECT_LE(v, 1.0f);
}

TEST(TextureInpaint, MaskTensorIsBinarised)
{
    // The model is trained on a hard 0/1 mask; passing soft grey through
    // measurably changes the fill. Any non-zero pixel must become exactly 1.
    QImage m(4, 1, QImage::Format_Grayscale8);
    m.scanLine(0)[0] = 0;
    m.scanLine(0)[1] = 1;
    m.scanLine(0)[2] = 127;
    m.scanLine(0)[3] = 255;

    const int t = TextureInpaint::kInputSize;
    const auto mt = TextureInpaint::toMaskTensor(m, t);
    ASSERT_EQ(mt.size(), static_cast<size_t>(t) * t);
    EXPECT_FLOAT_EQ(mt[0], 0.0f);
    EXPECT_FLOAT_EQ(mt[1], 1.0f);
    EXPECT_FLOAT_EQ(mt[2], 1.0f);
    EXPECT_FLOAT_EQ(mt[3], 1.0f);
}

TEST(TextureInpaint, TensorsAreAlwaysThePinnedSize)
{
    // The graph's spatial dims are pinned; a mis-sized tensor is rejected by
    // ORT at run time with a shape error (the #1016 BiRefNet field bug).
    const int t = TextureInpaint::kInputSize;
    for (const QSize s : {QSize(1, 1), QSize(64, 4000), QSize(1024, 1024)}) {
        QImage img(s, QImage::Format_RGB888);
        img.fill(Qt::gray);
        EXPECT_EQ(TextureInpaint::toImageTensor(img, t).size(),
                  static_cast<size_t>(t) * t * 3) << s.width() << "x" << s.height();
        QImage m(s, QImage::Format_Grayscale8);
        m.fill(255);
        EXPECT_EQ(TextureInpaint::toMaskTensor(m, t).size(),
                  static_cast<size_t>(t) * t) << s.width() << "x" << s.height();
    }
}

TEST(TextureInpaint, NullInputsStillYieldCorrectlyShapedTensors)
{
    const int t = TextureInpaint::kInputSize;
    EXPECT_EQ(TextureInpaint::toImageTensor(QImage(), t).size(),
              static_cast<size_t>(t) * t * 3);
    EXPECT_EQ(TextureInpaint::toMaskTensor(QImage(), t).size(),
              static_cast<size_t>(t) * t);
}

TEST(TextureInpaint, OutputIsNotRescaled)
{
    // The output is ALREADY 0..255. Dividing or multiplying by 255 here is the
    // mirror of the input trap and equally silent.
    const int w = 2, h = 1;
    std::vector<float> out(static_cast<size_t>(w) * h * 3, 0.0f);
    const size_t plane = static_cast<size_t>(w) * h;
    out[0] = 200.0f; out[plane] = 100.0f; out[2 * plane] = 0.0f;   // px0
    out[1] = 255.0f; out[plane + 1] = 0.0f; out[2 * plane + 1] = 50.0f; // px1

    const QImage img = TextureInpaint::fromOutputTensor(out, w, h, QImage());
    ASSERT_FALSE(img.isNull());
    EXPECT_EQ(img.pixelColor(0, 0), QColor(200, 100, 0));
    EXPECT_EQ(img.pixelColor(1, 0), QColor(255, 0, 50));
}

TEST(TextureInpaint, OutputClampsOutOfRangeValues)
{
    const int w = 2, h = 1;
    std::vector<float> out(static_cast<size_t>(w) * h * 3, 0.0f);
    const size_t plane = static_cast<size_t>(w) * h;
    out[0] = 300.0f;  out[plane] = -40.0f; out[2 * plane] = 128.0f;
    const QImage img = TextureInpaint::fromOutputTensor(out, w, h, QImage());
    ASSERT_FALSE(img.isNull());
    EXPECT_EQ(img.pixelColor(0, 0), QColor(255, 0, 128));
}

TEST(TextureInpaint, NonFiniteOutputFallsBackToTheSourcePixel)
{
    // The #1025 lesson: a numerically broken export returns finite-shaped,
    // non-finite output. Degrading to "unchanged" is recoverable; writing
    // NaN-derived garbage into a texture is not.
    const int w = 1, h = 1;
    std::vector<float> out(3);
    out[0] = std::numeric_limits<float>::quiet_NaN();
    out[1] = std::numeric_limits<float>::infinity();
    out[2] = -std::numeric_limits<float>::infinity();

    QImage src(1, 1, QImage::Format_RGB888);
    src.fill(QColor(10, 20, 30));

    const QImage img = TextureInpaint::fromOutputTensor(out, w, h, src);
    ASSERT_FALSE(img.isNull());
    EXPECT_EQ(img.pixelColor(0, 0), QColor(10, 20, 30));
}

TEST(TextureInpaint, OutputRejectsShortTensors)
{
    // Guard against reading past the end when a graph returns fewer values
    // than its declared shape.
    EXPECT_TRUE(TextureInpaint::fromOutputTensor({1.0f, 2.0f}, 4, 4, QImage())
                    .isNull());
    EXPECT_TRUE(TextureInpaint::fromOutputTensor({}, 0, 0, QImage()).isNull());
}

TEST(TextureInpaint, DilateGrowsTheMaskEightConnected)
{
    QImage m(5, 5, QImage::Format_Grayscale8);
    m.fill(0);
    m.scanLine(2)[2] = 255;   // single centre texel

    const QImage d1 = TextureInpaint::dilateMask(m, 1);
    EXPECT_EQ(TextureInpaint::countMasked(d1), 9);    // 3x3 block
    const QImage d2 = TextureInpaint::dilateMask(m, 2);
    EXPECT_EQ(TextureInpaint::countMasked(d2), 25);   // 5x5 block
}

TEST(TextureInpaint, DilateByZeroIsIdentity)
{
    QImage m(4, 4, QImage::Format_Grayscale8);
    m.fill(0);
    m.scanLine(1)[1] = 255;
    EXPECT_EQ(TextureInpaint::countMasked(TextureInpaint::dilateMask(m, 0)), 1);
    EXPECT_EQ(TextureInpaint::countMasked(TextureInpaint::dilateMask(m, -3)), 1);
}

TEST(TextureInpaint, CoverageMaskInvertsCoverage)
{
    // The bridge for consumer 1: a baker's coverage bitmap marks texels a chart
    // COVERS, and the ones to inpaint are exactly the others. Getting this
    // backwards would inpaint the whole model and erase the bake.
    const int w = 3, h = 2;
    const std::vector<uint8_t> covered = {1, 0, 1,
                                          0, 0, 1};
    const QImage m = TextureInpaint::maskFromCoverage(covered, w, h);
    ASSERT_FALSE(m.isNull());
    EXPECT_EQ(m.width(), w);
    EXPECT_EQ(m.height(), h);
    EXPECT_EQ(TextureInpaint::countMasked(m), 3);   // the three zeros
    EXPECT_EQ(m.constScanLine(0)[0], 0);            // covered → keep
    EXPECT_EQ(m.constScanLine(0)[1], 255);          // uncovered → fill
}

TEST(TextureInpaint, CoverageMaskSelectsTheGutterNotTheCharts)
{
    // The seam-fill contract end to end: a coverage bitmap with a covered
    // block and surrounding gutter must yield a mask that selects EXACTLY the
    // gutter. This is the assertion MeshGenBaker_test cannot make directly
    // (its dilatePx doubles as xatlas chart padding, so two bakes are not
    // comparable), so it is pinned here on the pure bridge instead.
    const int w = 8, h = 8;
    std::vector<uint8_t> covered(static_cast<size_t>(w) * h, 0);
    for (int y = 2; y < 6; ++y)
        for (int x = 2; x < 6; ++x)
            covered[static_cast<size_t>(y) * w + x] = 1;   // 4x4 chart

    const QImage m = TextureInpaint::maskFromCoverage(covered, w, h);
    ASSERT_FALSE(m.isNull());
    EXPECT_EQ(TextureInpaint::countMasked(m), w * h - 16);   // everything but the chart
    // The chart itself must be untouched — inpainting a baked chart would
    // discard real sampled colour.
    for (int y = 2; y < 6; ++y)
        for (int x = 2; x < 6; ++x)
            EXPECT_EQ(m.constScanLine(y)[x], 0) << x << "," << y;
    // ...and the gutter must be selected.
    EXPECT_EQ(m.constScanLine(0)[0], 255);
    EXPECT_EQ(m.constScanLine(7)[7], 255);
}

TEST(TextureInpaint, CoverageMaskRejectsShortInput)
{
    EXPECT_TRUE(TextureInpaint::maskFromCoverage({1, 0}, 4, 4).isNull());
    EXPECT_TRUE(TextureInpaint::maskFromCoverage({}, 0, 0).isNull());
}

TEST(TextureInpaint, CountMaskedHandlesNullAndEmpty)
{
    EXPECT_EQ(TextureInpaint::countMasked(QImage()), 0);
    QImage m(4, 4, QImage::Format_Grayscale8);
    m.fill(0);
    EXPECT_EQ(TextureInpaint::countMasked(m), 0);
    m.fill(255);
    EXPECT_EQ(TextureInpaint::countMasked(m), 16);
}

#ifndef ENABLE_ONNX
TEST(TextureInpaint, NonOnnxBuildReportsWhyInsteadOfPassingThrough)
{
    // A silent pass-through would present as "the inpaint did nothing".
    QImage tex(8, 8, QImage::Format_RGB888);
    tex.fill(Qt::gray);
    QImage m(8, 8, QImage::Format_Grayscale8);
    m.fill(255);

    const auto r = TextureInpaint::inpaint(tex, m, QStringLiteral("/nope.onnx"));
    EXPECT_FALSE(r.ok);
    EXPECT_FALSE(r.error.isEmpty());
    EXPECT_FALSE(TextureInpaint::isAvailable());
}
#endif
