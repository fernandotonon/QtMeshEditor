// Unit tests for TextureUpscaler (#405). GL-free; the actual ONNX inference
// needs a downloaded model, so these cover the error contracts that don't
// require one (and don't touch the network).

#include <gtest/gtest.h>

#include <QImage>

#include "TextureUpscaler.h"

namespace {
QImage solid(int w, int h, int v) { QImage i(w, h, QImage::Format_RGB888); i.fill(qRgb(v, v, v)); return i; }
}

// A null source is rejected (ENABLE_ONNX) / the not-built error fires (otherwise).
TEST(TextureUpscalerCore, NullSourceFails)
{
    TextureUpscaler::Result r = TextureUpscaler::upscale(QImage(), "/no/model.onnx", {});
    EXPECT_FALSE(r.ok);
    EXPECT_FALSE(r.error.isEmpty());
}

// A valid image but a nonexistent model path fails gracefully (no crash).
TEST(TextureUpscalerCore, MissingModelFailsGracefully)
{
    TextureUpscaler::Result r =
        TextureUpscaler::upscale(solid(16, 16, 128), "/nonexistent/realesrgan.onnx", {});
    EXPECT_FALSE(r.ok);
    EXPECT_FALSE(r.error.isEmpty());
    EXPECT_TRUE(r.image.isNull());
}

#ifndef ENABLE_ONNX
// Without ONNX the error explicitly points at the build flag.
TEST(TextureUpscalerCore, NotBuiltErrorMentionsFlag)
{
    TextureUpscaler::Result r = TextureUpscaler::upscale(solid(8, 8, 50), "x.onnx", {});
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(r.error.contains("ENABLE_ONNX"));
}
#endif
