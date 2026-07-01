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

// Full segmentation only when ONNX + model present.
TEST(BackgroundRemoverTest, SegmentsWhenModelPresent)
{
#ifndef ENABLE_ONNX
    GTEST_SKIP() << "built without ENABLE_ONNX";
#else
    if (!QFileInfo::exists(BackgroundRemover::modelPath()))
        GTEST_SKIP() << "u2net model not present in the AppData cache";
    QImage img(128, 128, QImage::Format_RGB888);
    img.fill(Qt::white);
    // A dark square in the middle = a salient object over white.
    for (int y = 40; y < 88; ++y)
        for (int x = 40; x < 88; ++x)
            img.setPixel(x, y, qRgb(20, 20, 20));
    auto r = BackgroundRemover::removeBackground(img, BackgroundRemover::modelPath(), {});
    if (r.ok) {
        EXPECT_TRUE(r.usedModel);
        EXPECT_EQ(r.image.size(), img.size());
    }
#endif
}
