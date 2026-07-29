#include <gtest/gtest.h>

#include "PaintLayerBlend.h"
#include "PaintLayerStack.h"
#include "TexturePaintBuffer.h"

#include <cmath>
#include <vector>

namespace {

PaintLayerBlend::Rgba px(const std::vector<uint8_t>& buf, int w, int x, int y)
{
    const size_t off = (static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)) * 4u;
    return PaintLayerBlend::rgbaFromBytes(buf[off], buf[off + 1], buf[off + 2], buf[off + 3]);
}

void setLayerPixel(TexturePaintBuffer& buf, int x, int y,
                   uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    buf.setPixel(x, y, Ogre::ColourValue(
        PaintLayerBlend::byteToF(r),
        PaintLayerBlend::byteToF(g),
        PaintLayerBlend::byteToF(b),
        PaintLayerBlend::byteToF(a)));
}

} // namespace

TEST(PaintLayerBlendTest, NormalOpacityComposite)
{
    const PaintLayerBlend::Rgba dst{0.2f, 0.2f, 0.2f, 1.f};
    const PaintLayerBlend::Rgba src{1.f, 0.f, 0.f, 0.5f};
    const auto out = PaintLayerBlend::blendPixel(dst, src, PaintLayerBlend::Mode::Normal, 1.f);
    EXPECT_NEAR(out.r, 0.6f, 0.02f);
    EXPECT_NEAR(out.g, 0.1f, 0.02f);
}

TEST(PaintLayerBlendTest, MultiplyMode)
{
    const PaintLayerBlend::Rgba dst{0.5f, 0.5f, 0.5f, 1.f};
    const PaintLayerBlend::Rgba src{0.5f, 1.f, 0.f, 1.f};
    const auto out = PaintLayerBlend::blendPixel(dst, src, PaintLayerBlend::Mode::Multiply, 1.f);
    EXPECT_NEAR(out.r, 0.25f, 0.02f);
    EXPECT_NEAR(out.g, 0.5f, 0.02f);
    EXPECT_NEAR(out.b, 0.f, 0.02f);
}

TEST(PaintLayerBlendTest, ScreenMode)
{
    const PaintLayerBlend::Rgba dst{0.2f, 0.2f, 0.2f, 1.f};
    const PaintLayerBlend::Rgba src{0.5f, 0.5f, 0.5f, 1.f};
    const auto out = PaintLayerBlend::blendPixel(dst, src, PaintLayerBlend::Mode::Screen, 1.f);
    EXPECT_GT(out.r, 0.5f);
}

TEST(PaintLayerBlendTest, LayerMaskReducesContribution)
{
    const PaintLayerBlend::Rgba dst{0.f, 0.f, 0.f, 1.f};
    const PaintLayerBlend::Rgba src{1.f, 0.f, 0.f, 1.f};
    const auto full = PaintLayerBlend::blendPixel(dst, src, PaintLayerBlend::Mode::Normal, 1.f, 255);
    const auto half = PaintLayerBlend::blendPixel(dst, src, PaintLayerBlend::Mode::Normal, 1.f, 128);
    EXPECT_NEAR(full.r, 1.f, 0.02f);
    EXPECT_NEAR(half.r, 0.5f, 0.04f);
}

TEST(PaintLayerBlendTest, ThreeLayerFixtureAllBlendModes)
{
    constexpr int W = 4;
    constexpr int H = 1;
    std::vector<uint8_t> bottom(W * H * 4, 0);
    std::vector<uint8_t> mid(W * H * 4, 0);
    std::vector<uint8_t> top(W * H * 4, 0);
    for (int i = 0; i < W; ++i) {
        bottom[i * 4 + 0] = 64;
        bottom[i * 4 + 1] = 64;
        bottom[i * 4 + 2] = 64;
        bottom[i * 4 + 3] = 255;
        mid[i * 4 + 0] = 128;
        mid[i * 4 + 1] = 0;
        mid[i * 4 + 2] = 0;
        mid[i * 4 + 3] = 255;
        top[i * 4 + 0] = 0;
        top[i * 4 + 1] = 128;
        top[i * 4 + 2] = 0;
        top[i * 4 + 3] = 128;
    }

    const PaintLayerBlend::Mode modes[] = {
        PaintLayerBlend::Mode::Normal,
        PaintLayerBlend::Mode::Multiply,
        PaintLayerBlend::Mode::Screen,
        PaintLayerBlend::Mode::Overlay,
        PaintLayerBlend::Mode::Add,
        PaintLayerBlend::Mode::Subtract,
        PaintLayerBlend::Mode::SoftLight,
        PaintLayerBlend::Mode::Hue,
    };

    for (auto mode : modes) {
        std::vector<PaintLayerBlend::LayerInput> layers(3);
        layers[0] = {bottom.data(), nullptr, PaintLayerBlend::Mode::Normal, 1.f, true};
        layers[1] = {mid.data(), nullptr, mode, 1.f, true};
        layers[2] = {top.data(), nullptr, PaintLayerBlend::Mode::Normal, 0.5f, true};
        std::vector<uint8_t> out;
        PaintLayerBlend::compositeLayers(W, H, layers, out);
        ASSERT_EQ(out.size(), static_cast<size_t>(W * H * 4));
        const auto c = px(out, W, 0, 0);
        EXPECT_GE(c.r, 0.f);
        EXPECT_LE(c.r, 1.f);
        EXPECT_GE(c.g, 0.f);
        EXPECT_LE(c.g, 1.f);
    }
}

TEST(PaintLayerBlendTest, CompositeRegionMatchesFull)
{
    const int w = 4;
    const int h = 4;
    std::vector<uint8_t> red(w * h * 4, 0);
    for (int i = 0; i < w * h; ++i) {
        red[i * 4u + 0] = 255;
        red[i * 4u + 3] = 255;
    }
    std::vector<uint8_t> blue(w * h * 4, 0);
    for (int y = 1; y < 3; ++y)
        for (int x = 1; x < 3; ++x) {
            const size_t off = (static_cast<size_t>(y) * static_cast<size_t>(w)
                                + static_cast<size_t>(x)) * 4u;
            blue[off + 2] = 255;
            blue[off + 3] = 128;
        }

    std::vector<PaintLayerBlend::LayerInput> layers = {
        {red.data(), nullptr, PaintLayerBlend::Mode::Normal, 1.f, true},
        {blue.data(), nullptr, PaintLayerBlend::Mode::Normal, 1.f, true},
    };

    std::vector<uint8_t> full;
    PaintLayerBlend::compositeLayers(w, h, layers, full);

    std::vector<uint8_t> region = full;
    PaintLayerBlend::compositeLayersRegion(w, h, layers, region.data(), 1, 1, 3, 3);

    for (int y = 1; y < 3; ++y) {
        for (int x = 1; x < 3; ++x) {
            EXPECT_FLOAT_EQ(px(full, w, x, y).b, px(region, w, x, y).b);
            EXPECT_FLOAT_EQ(px(full, w, x, y).r, px(region, w, x, y).r);
        }
    }
}

TEST(PaintLayerStackTest, InitFromFlatBufferCreatesLayer0)
{
    TexturePaintBuffer flat(2, 2);
    flat.clear(Ogre::ColourValue(0.5f, 0.25f, 0.f, 1.f));
    flat.clearDirty();

    PaintLayerStack stack;
    stack.initFromFlatBuffer(flat);
    EXPECT_EQ(stack.layerCount(), 1);
    EXPECT_EQ(stack.layer(0).name, QStringLiteral("Layer 0"));
    EXPECT_EQ(stack.layer(0).buffer.width(), 2);
}

TEST(PaintLayerStackTest, SoloShowsOnlyOneLayer)
{
    PaintLayerStack stack;
    TexturePaintBuffer red(2, 2);
    red.clear(Ogre::ColourValue(1.f, 0.f, 0.f, 1.f));
    red.clearDirty();
    stack.initFromFlatBuffer(red);

    stack.addEmpty(QStringLiteral("Blue"));
    setLayerPixel(stack.layer(1).buffer, 0, 0, 0, 0, 255, 255);

    std::vector<uint8_t> composite;
    stack.compositeTo(composite);
    auto cBoth = px(composite, 2, 0, 0);
    EXPECT_GT(cBoth.b, 0.5f); // blue layer visible in stack

    stack.setSolo(0, true);
    stack.compositeTo(composite);
    auto cSolo = px(composite, 2, 0, 0);
    EXPECT_GT(cSolo.r, 0.9f);
    EXPECT_LT(cSolo.b, 0.1f);
}

TEST(PaintLayerStackTest, MergeDownCombinesLayers)
{
    PaintLayerStack stack;
    TexturePaintBuffer base(2, 2);
    base.clear(Ogre::ColourValue(1.f, 1.f, 1.f, 1.f));
    base.clearDirty();
    stack.initFromFlatBuffer(base);
    stack.addEmpty(QStringLiteral("Paint"));
    setLayerPixel(stack.activeLayer().buffer, 0, 0, 255, 0, 0, 255);

    stack.mergeDown(1);
    EXPECT_EQ(stack.layerCount(), 1);
    const auto c = stack.layer(0).buffer.pixel(0, 0);
    EXPECT_GT(c.r, 0.5f);
}

TEST(PaintLayerStackTest, LayerMaskHidesStrokeRegion)
{
    PaintLayerStack stack;
    TexturePaintBuffer base(4, 4);
    base.clear(Ogre::ColourValue(0.f, 0.f, 0.f, 1.f));
    base.clearDirty();
    stack.initFromFlatBuffer(base);

    auto& mask = stack.ensureLayerMask(0);
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 2; ++x)
            mask[static_cast<size_t>(y) * 4u + static_cast<size_t>(x)] = 0;

    setLayerPixel(stack.layer(0).buffer, 0, 0, 255, 255, 255, 255);
    setLayerPixel(stack.layer(0).buffer, 3, 2, 255, 255, 255, 255);

    std::vector<uint8_t> out;
    stack.compositeTo(out);
    auto hidden = px(out, 4, 0, 0);
    auto shown = px(out, 4, 3, 2);
    EXPECT_LT(hidden.r, 0.1f);
    EXPECT_GT(shown.r, 0.9f);
}
