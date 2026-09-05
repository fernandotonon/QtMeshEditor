#include "BackgroundRemover.h"

#include <gtest/gtest.h>

#include <QFileInfo>
#include <QImage>

// Tests for BackgroundRemover (epic #764, U²-Net). The ONNX inference path needs
// the model + a GL-free ORT run; the always-compiled paths (availability, model
// path, graceful no-model fallback) are tested unconditionally.

TEST(BackgroundRemoverTest, IsAvailableReflectsOnnxBuild)
{
#ifdef ENABLE_ONNX
    EXPECT_TRUE(BackgroundRemover::isAvailable());
#else
    EXPECT_FALSE(BackgroundRemover::isAvailable());
#endif
}

TEST(BackgroundRemoverTest, ModelPathIsUnderRembgCache)
{
    EXPECT_TRUE(BackgroundRemover::modelPath().contains("ai_models/rembg"));
    EXPECT_TRUE(BackgroundRemover::modelPath().endsWith("u2net.onnx"));
}

TEST(BackgroundRemoverTest, WithoutModelReturnsOriginalImage)
{
    // The contract: on any failure (no ONNX / missing model) return ok=false but
    // hand back the ORIGINAL image so the caller can proceed.
    QImage img(32, 24, QImage::Format_RGB888);
    img.fill(Qt::green);
    auto r = BackgroundRemover::removeBackground(img, "/no/such/u2net.onnx", {});
    EXPECT_FALSE(r.ok);
    EXPECT_FALSE(r.error.isEmpty());
    ASSERT_FALSE(r.image.isNull());
    EXPECT_EQ(r.image.width(), 32);
    EXPECT_EQ(r.image.height(), 24);
}

TEST(BackgroundRemoverTest, NullImageIsSafe)
{
    auto r = BackgroundRemover::removeBackground(QImage(), BackgroundRemover::modelPath(), {});
    EXPECT_FALSE(r.ok);
}

// Full segmentation when the model is present; otherwise the graceful-fallback
// contract. Does NOT GTEST_SKIP — CI treats skipped tests as failures and the
// u2net model isn't hosted yet (#769), so this must assert on every runner.
TEST(BackgroundRemoverTest, SegmentsWhenModelPresentElseFallsBack)
{
    QImage img(128, 128, QImage::Format_RGB888);
    img.fill(Qt::white);
    // A dark square in the middle = a salient object over white.
    for (int y = 40; y < 88; ++y)
        for (int x = 40; x < 88; ++x)
            img.setPixel(x, y, qRgb(20, 20, 20));

    auto r = BackgroundRemover::removeBackground(img, BackgroundRemover::modelPath(), {});
    const bool present = BackgroundRemover::isAvailable()
                         && QFileInfo::exists(BackgroundRemover::modelPath());
    if (present && r.ok) {
        EXPECT_TRUE(r.usedModel);
        EXPECT_EQ(r.image.size(), img.size());
    } else {
        // No ONNX / no model: contract is ok=false + the ORIGINAL image returned.
        EXPECT_FALSE(r.ok);
        EXPECT_FALSE(r.error.isEmpty());
        EXPECT_EQ(r.image.size(), img.size());   // original passed through
    }
}

// --- Uniform-background rescue (pure, model-free) --------------------------
// The fixture: a white 200×200 backdrop with a blue "body" the saliency
// already found (alpha=1). The rescue must recover geometry the saliency
// missed WITHOUT re-mattering the backdrop (3.37.3 field reports: kept
// shadows/vignettes became flat slabs and washed the bakes out bright).

namespace {

struct RescueFixture {
    static constexpr int W = 200, H = 200;
    QImage img{W, H, QImage::Format_RGB32};
    std::vector<float> alpha;

    RescueFixture()
    {
        img.fill(qRgb(255, 255, 255));
        alpha.assign(size_t(W) * H, 0.0f);
        // Body: blue rect, already matted as foreground.
        for (int y = 80; y <= 160; ++y)
            for (int x = 30; x <= 90; ++x) {
                img.setPixel(x, y, qRgb(40, 60, 200));
                alpha[size_t(y) * W + x] = 1.0f;
            }
    }
    float a(int x, int y) const { return alpha[size_t(y) * W + x]; }
};

} // namespace

TEST(BackgroundRemoverTest, RescueRecoversLimbConnectedToSubject)
{
    RescueFixture f;
    // A thin blue "arm" the saliency missed, attached to the body.
    for (int y = 100; y <= 110; ++y)
        for (int x = 91; x <= 170; ++x)
            f.img.setPixel(x, y, qRgb(40, 60, 200));

    BackgroundRemover::applyUniformBackgroundRescue(f.img, f.alpha);
    EXPECT_EQ(f.a(160, 105), 1.0f);   // arm tip rescued
    EXPECT_EQ(f.a(120, 105), 1.0f);   // arm middle rescued
    EXPECT_EQ(f.a(180, 30), 0.0f);    // plain backdrop untouched
}

TEST(BackgroundRemoverTest, RescueIgnoresDropShadow)
{
    RescueFixture f;
    // A gray drop shadow touching the body's feet: same chroma as the white
    // backdrop, darker — the old rescue kept it and TRELLIS grew a slab.
    for (int y = 161; y <= 175; ++y)
        for (int x = 30; x <= 105; ++x)
            f.img.setPixel(x, y, qRgb(170, 170, 170));

    BackgroundRemover::applyUniformBackgroundRescue(f.img, f.alpha);
    EXPECT_EQ(f.a(60, 175), 0.0f);    // shadow stays background
    EXPECT_EQ(f.a(105, 170), 0.0f);
}

TEST(BackgroundRemoverTest, RescueIgnoresDeepClippedShadow)
{
    RescueFixture f;
    // A deep, near-clipped shadow (below the proportional-shading window,
    // k >= 3.5) touching the body: achromatic on an achromatic backdrop is
    // still shading, however dark. Dark GEOMETRY on a white backdrop is the
    // saliency net's job — solid dark masses are salient.
    for (int y = 161; y <= 175; ++y)
        for (int x = 30; x <= 105; ++x)
            f.img.setPixel(x, y, qRgb(45, 45, 45));

    BackgroundRemover::applyUniformBackgroundRescue(f.img, f.alpha);
    EXPECT_EQ(f.a(60, 175), 0.0f);    // deep shadow stays background
    // A CHROMATIC dark limb at the same depth is still rescued.
    for (int y = 100; y <= 110; ++y)
        for (int x = 91; x <= 170; ++x)
            f.img.setPixel(x, y, qRgb(20, 25, 90));
    BackgroundRemover::applyUniformBackgroundRescue(f.img, f.alpha);
    EXPECT_EQ(f.a(160, 105), 1.0f);
}

TEST(BackgroundRemoverTest, RescueIgnoresDisconnectedBlob)
{
    RescueFixture f;
    // A strongly-colored watermark/logo far from the subject: chroma passes,
    // but it is not connected to the saliency foreground.
    for (int y = 15; y <= 40; ++y)
        for (int x = 150; x <= 190; ++x)
            f.img.setPixel(x, y, qRgb(200, 40, 40));

    BackgroundRemover::applyUniformBackgroundRescue(f.img, f.alpha);
    EXPECT_EQ(f.a(170, 25), 0.0f);    // disconnected blob dropped
}

TEST(BackgroundRemoverTest, RescueAbortsWhenItWouldFloodTheFrame)
{
    RescueFixture f;
    // Corners agree (white — the panel stays clear of all four 24px corner
    // patches) but the "background" isn't uniform after all: a huge colored
    // panel touches the body. Rescuing it would hand the 3D generation a
    // backdrop slab — the safety valve must bail out instead.
    for (int y = 30; y <= 165; ++y)
        for (int x = 91; x <= 190; ++x)
            f.img.setPixel(x, y, qRgb(80, 160, 90));

    BackgroundRemover::applyUniformBackgroundRescue(f.img, f.alpha);
    EXPECT_EQ(f.a(150, 120), 0.0f);   // panel not rescued
    EXPECT_EQ(f.a(60, 120), 1.0f);    // body untouched
}

TEST(BackgroundRemoverTest, RescueSkipsBusyBackground)
{
    RescueFixture f;
    // Make the corners disagree — no reliable background color, no rescue.
    for (int y = 0; y < 30; ++y)
        for (int x = 0; x < 30; ++x)
            f.img.setPixel(x, y, qRgb(200, 30, 30));
    // A missed limb that WOULD be rescued on a uniform backdrop.
    for (int y = 100; y <= 110; ++y)
        for (int x = 91; x <= 170; ++x)
            f.img.setPixel(x, y, qRgb(40, 60, 200));

    BackgroundRemover::applyUniformBackgroundRescue(f.img, f.alpha);
    EXPECT_EQ(f.a(160, 105), 0.0f);   // gate closed → alpha untouched
}
