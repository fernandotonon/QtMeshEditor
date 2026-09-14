#include <gtest/gtest.h>

#include "PhotoDepth.h"

#include <QImage>
#include <QPainter>

#include <cmath>
#include <limits>
#include <vector>

// PhotoDepth's pre/post-processing is pure data (no Ogre, no GL, no ONNX
// session), so these run on every build. The ONNX inference itself needs the
// hosted model and is exercised manually; what is pinned here is the CONTRACT
// the model relies on — get the normalisation or the near/far convention wrong
// and the depth map is silently useless as a ControlNet input.

TEST(PhotoDepth, ModelInputIsImageNetNormalisedCHW)
{
    QImage img(64, 32, QImage::Format_RGB888);
    img.fill(QColor(255, 0, 0));   // pure red

    const auto in = PhotoDepth::toModelInput(img, /*letterbox=*/false);
    ASSERT_EQ(in.size(), static_cast<size_t>(PhotoDepth::kInputSize)
                             * PhotoDepth::kInputSize * 3);

    // Channel-major (CHW), not interleaved: the whole R plane comes first.
    const size_t plane = static_cast<size_t>(PhotoDepth::kInputSize)
                         * PhotoDepth::kInputSize;
    // R = (1.0 - 0.485) / 0.229, G = (0 - 0.456) / 0.224, B = (0 - 0.406)/0.225
    EXPECT_NEAR(in[0], (1.0f - 0.485f) / 0.229f, 1e-3f);
    EXPECT_NEAR(in[plane], (0.0f - 0.456f) / 0.224f, 1e-3f);
    EXPECT_NEAR(in[2 * plane], (0.0f - 0.406f) / 0.225f, 1e-3f);
}

TEST(PhotoDepth, ModelInputIsAlwaysThePinnedSize)
{
    // The export pins H/W to 518 (a dynamic-axis graph drifts badly away from
    // the traced size), so ANY source must come out at exactly that size.
    for (const QSize s : {QSize(16, 16), QSize(1024, 768), QSize(7, 913)}) {
        QImage img(s, QImage::Format_RGB888);
        img.fill(Qt::gray);
        EXPECT_EQ(PhotoDepth::toModelInput(img, false).size(),
                  static_cast<size_t>(PhotoDepth::kInputSize)
                      * PhotoDepth::kInputSize * 3)
            << "source " << s.width() << "x" << s.height();
    }
}

TEST(PhotoDepth, NullImageYieldsZeroedInputNotACrash)
{
    const auto in = PhotoDepth::toModelInput(QImage(), false);
    EXPECT_EQ(in.size(), static_cast<size_t>(PhotoDepth::kInputSize)
                             * PhotoDepth::kInputSize * 3);
    for (float v : in) EXPECT_FLOAT_EQ(v, 0.0f);
}

TEST(PhotoDepth, DepthImageKeepsNearBright)
{
    // The model emits INVERSE depth (larger = nearer) and MeshDepthRenderer
    // uses near=bright, so the mapping must NOT invert. Flipping this produces
    // a map that looks plausible but conditions ControlNet backwards.
    std::vector<float> raw = {0.0f, 1.0f, 2.0f, 3.0f};
    float mn = 0, mx = 0;
    const QImage img = PhotoDepth::toDepthImage(raw, 2, 2, &mn, &mx);
    ASSERT_FALSE(img.isNull());
    EXPECT_FLOAT_EQ(mn, 0.0f);
    EXPECT_FLOAT_EQ(mx, 3.0f);
    EXPECT_EQ(qGray(img.pixel(0, 0)), 0);     // smallest value -> darkest
    EXPECT_EQ(qGray(img.pixel(1, 1)), 255);   // largest value  -> brightest
    EXPECT_GT(qGray(img.pixel(1, 0)), qGray(img.pixel(0, 0)));
}

TEST(PhotoDepth, ConstantFieldBecomesMidGreyNotNaN)
{
    // A flat field would divide by zero when normalising. Emitting a NaN-filled
    // image would then propagate silently into the texture pipeline.
    std::vector<float> raw(16, 2.5f);
    const QImage img = PhotoDepth::toDepthImage(raw, 4, 4);
    ASSERT_FALSE(img.isNull());
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x)
            EXPECT_EQ(qGray(img.pixel(x, y)), 128);
}

TEST(PhotoDepth, NonFiniteValuesDoNotPoisonTheRange)
{
    // One stray NaN must not drag min/max to NaN and blank the whole map.
    // NaN FIRST: std::min/max propagate a leading NaN through every later
    // comparison, so this ordering is what actually exercises the guard. With
    // NaN in the middle the comparisons happen to survive and the test passes
    // even unguarded (verified by mutation).
    std::vector<float> raw = {std::numeric_limits<float>::quiet_NaN(),
                              0.0f, 1.0f, 2.0f};
    float mn = 0, mx = 0;
    const QImage img = PhotoDepth::toDepthImage(raw, 2, 2, &mn, &mx);
    ASSERT_FALSE(img.isNull());
    EXPECT_TRUE(std::isfinite(mn));
    EXPECT_TRUE(std::isfinite(mx));
    EXPECT_FLOAT_EQ(mn, 0.0f);
    EXPECT_FLOAT_EQ(mx, 2.0f);
    EXPECT_EQ(qGray(img.pixel(1, 1)), 255);   // the real max still maps to white
    // The NaN TEXEL must land on a defined value. Without the per-pixel guard
    // the NaN flows into the 0..255 cast, which is undefined behaviour — in
    // practice it lands on 0 on this toolchain, so assert the defined value the
    // guard produces (mid-grey, the same neutral a flat field maps to) rather
    // than merely "in range", which UB happens to satisfy.
    EXPECT_EQ(qGray(img.pixel(0, 0)), 128)
        << "a non-finite texel must map to the neutral mid-grey";
}

TEST(PhotoDepth, DepthImageRejectsUndersizedInput)
{
    std::vector<float> raw = {0.0f, 1.0f};
    EXPECT_TRUE(PhotoDepth::toDepthImage(raw, 4, 4).isNull());
    EXPECT_TRUE(PhotoDepth::toDepthImage(raw, 0, 0).isNull());
}

TEST(PhotoDepth, CropLetterboxRecoversTheSourceAspect)
{
    // A 2:1 source letterboxed into a square must crop back to 2:1.
    QImage square(100, 100, QImage::Format_Grayscale8);
    square.fill(0);
    const QImage cropped = PhotoDepth::cropLetterbox(square, 200, 100);
    EXPECT_EQ(cropped.width(), 100);
    EXPECT_EQ(cropped.height(), 50);
    // A square source is returned untouched.
    EXPECT_EQ(PhotoDepth::cropLetterbox(square, 100, 100).size(), square.size());
}

TEST(PhotoDepth, LetterboxPreservesAspectByPadding)
{
    // A wide image letterboxed into the square input must leave black bars,
    // i.e. the top row is padding while the middle row carries image data.
    QImage wide(200, 50, QImage::Format_RGB888);
    wide.fill(QColor(255, 255, 255));
    const auto in = PhotoDepth::toModelInput(wide, /*letterbox=*/true);
    const size_t W = PhotoDepth::kInputSize;
    // Row 0 is padding -> normalised black, i.e. (0 - mean)/std (negative).
    EXPECT_LT(in[0], 0.0f);
    // The centre row is white -> (1 - mean)/std (positive).
    EXPECT_GT(in[(W / 2) * W + W / 2], 0.0f);
}

TEST(PhotoDepth, EstimateFailsCleanlyWithoutAModel)
{
    QImage img(32, 32, QImage::Format_RGB888);
    img.fill(Qt::gray);
    const auto r = PhotoDepth::estimate(img, QStringLiteral("/no/such/model.onnx"));
    EXPECT_FALSE(r.ok);
    EXPECT_FALSE(r.error.isEmpty());
    EXPECT_TRUE(r.depth.isNull()) << "a failed estimate must not return a map";
}

TEST(PhotoDepth, EstimateRejectsAnEmptyImage)
{
    const auto r = PhotoDepth::estimate(QImage(), PhotoDepth::modelPath());
    EXPECT_FALSE(r.ok);
    EXPECT_FALSE(r.error.isEmpty());
}

TEST(PhotoDepth, ModelPathLivesUnderTheDepthCache)
{
    EXPECT_TRUE(PhotoDepth::modelPath().contains(QStringLiteral("ai_models")));
    EXPECT_TRUE(PhotoDepth::modelPath().contains(QStringLiteral("depth")));
    EXPECT_TRUE(PhotoDepth::modelPath().endsWith(QStringLiteral("da2_small.onnx")));
}
