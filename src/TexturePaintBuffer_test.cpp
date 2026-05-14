#include <gtest/gtest.h>

#include <QImage>
#include <QTemporaryDir>

#include "TexturePaintBuffer.h"

namespace {

constexpr uint8_t kFull = 255;

uint8_t byte(int x, int y, int width, const std::vector<uint8_t>& data, int channel)
{
    const size_t off = (static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)) * 4u;
    return data[off + static_cast<size_t>(channel)];
}

} // namespace

TEST(TexturePaintBufferTest, DefaultInitOpaqueWhite)
{
    TexturePaintBuffer buf(4, 4);
    EXPECT_EQ(buf.width(), 4);
    EXPECT_EQ(buf.height(), 4);
    // 4x4 RGBA = 64 bytes, all 0xFF
    EXPECT_EQ(buf.data().size(), 64u);
    for (uint8_t b : buf.data())
        EXPECT_EQ(b, kFull);
    EXPECT_TRUE(buf.dirtyRect().empty());
}

TEST(TexturePaintBufferTest, ResizeClearsBufferAndDirtyRect)
{
    TexturePaintBuffer buf(2, 2);
    buf.setPixel(0, 0, Ogre::ColourValue(0, 0, 0, 1));
    EXPECT_FALSE(buf.dirtyRect().empty());
    buf.resize(8, 4);
    EXPECT_EQ(buf.width(), 8);
    EXPECT_EQ(buf.height(), 4);
    EXPECT_TRUE(buf.dirtyRect().empty());
    EXPECT_EQ(buf.pixel(0, 0).r, 1.0f);  // post-resize is opaque white
}

TEST(TexturePaintBufferTest, ClearFillsBufferAndMarksFullDirty)
{
    TexturePaintBuffer buf(4, 4);
    buf.clear(Ogre::ColourValue(0.5f, 0.0f, 1.0f, 1.0f));
    EXPECT_EQ(buf.dirtyRect().x0, 0);
    EXPECT_EQ(buf.dirtyRect().y0, 0);
    EXPECT_EQ(buf.dirtyRect().x1, 4);
    EXPECT_EQ(buf.dirtyRect().y1, 4);
    EXPECT_NEAR(buf.pixel(2, 2).r, 0.5f, 0.01f);
    EXPECT_NEAR(buf.pixel(2, 2).b, 1.0f, 0.01f);
}

TEST(TexturePaintBufferTest, SetPixelExpandsDirtyRect)
{
    TexturePaintBuffer buf(8, 8);
    buf.setPixel(2, 3, Ogre::ColourValue::Red);
    EXPECT_EQ(buf.dirtyRect().x0, 2);
    EXPECT_EQ(buf.dirtyRect().y0, 3);
    EXPECT_EQ(buf.dirtyRect().x1, 3);
    EXPECT_EQ(buf.dirtyRect().y1, 4);

    buf.setPixel(5, 6, Ogre::ColourValue::Blue);
    EXPECT_EQ(buf.dirtyRect().x0, 2);
    EXPECT_EQ(buf.dirtyRect().y0, 3);
    EXPECT_EQ(buf.dirtyRect().x1, 6);
    EXPECT_EQ(buf.dirtyRect().y1, 7);
}

TEST(TexturePaintBufferTest, SetPixelOutOfBoundsIsNoop)
{
    TexturePaintBuffer buf(4, 4);
    buf.setPixel(-1, -1, Ogre::ColourValue::Red);
    buf.setPixel(4, 4, Ogre::ColourValue::Red);
    EXPECT_TRUE(buf.dirtyRect().empty());
    // All pixels still opaque white.
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x)
            EXPECT_EQ(byte(x, y, 4, buf.data(), 0), kFull);
}

TEST(TexturePaintBufferTest, ClearDirtyResets)
{
    TexturePaintBuffer buf(4, 4);
    buf.setPixel(0, 0, Ogre::ColourValue::Red);
    EXPECT_FALSE(buf.dirtyRect().empty());
    buf.clearDirty();
    EXPECT_TRUE(buf.dirtyRect().empty());
}

TEST(TexturePaintBufferTest, PaintBrushFullStrengthFillsCenterPixel)
{
    TexturePaintBuffer buf(32, 32);
    // Brush at UV center, radius covering ~6 pixels horizontally.
    const int painted = buf.paintBrush(Ogre::Vector2(0.5f, 0.5f),
                                       0.1f,
                                       Ogre::ColourValue::Red,
                                       1.0f, 0.0f);
    EXPECT_GT(painted, 0);
    // Center pixel must be ~red.
    int cx = 0, cy = 0;
    buf.uvToPixel(Ogre::Vector2(0.5f, 0.5f), cx, cy);
    EXPECT_NEAR(buf.pixel(cx, cy).r, 1.0f, 0.02f);
    EXPECT_NEAR(buf.pixel(cx, cy).g, 0.0f, 0.02f);
    // Pixel far outside brush is untouched white.
    EXPECT_EQ(byte(0, 0, 32, buf.data(), 0), kFull);
    EXPECT_EQ(byte(0, 0, 32, buf.data(), 1), kFull);
}

TEST(TexturePaintBufferTest, PaintBrushDirtyRectContainsAffectedPixels)
{
    TexturePaintBuffer buf(64, 64);
    const int painted = buf.paintBrush(Ogre::Vector2(0.5f, 0.5f),
                                       0.1f,
                                       Ogre::ColourValue::Green,
                                       1.0f, 0.5f);
    EXPECT_GT(painted, 0);
    const auto& d = buf.dirtyRect();
    EXPECT_FALSE(d.empty());
    // The brush is centered at ~(32, 32). The dirty rect must include it.
    EXPECT_LE(d.x0, 32);
    EXPECT_GE(d.x1, 32);
    EXPECT_LE(d.y0, 32);
    EXPECT_GE(d.y1, 32);
    // And must NOT span the entire buffer.
    EXPECT_LT(d.width(), 64);
    EXPECT_LT(d.height(), 64);
}

TEST(TexturePaintBufferTest, PaintBrushZeroStrengthIsNoop)
{
    TexturePaintBuffer buf(16, 16);
    const int painted = buf.paintBrush(Ogre::Vector2(0.5f, 0.5f),
                                       0.2f,
                                       Ogre::ColourValue::Red,
                                       0.0f, 0.5f);
    EXPECT_EQ(painted, 0);
    EXPECT_TRUE(buf.dirtyRect().empty());
}

TEST(TexturePaintBufferTest, PaintBrushZeroRadiusIsNoop)
{
    TexturePaintBuffer buf(16, 16);
    const int painted = buf.paintBrush(Ogre::Vector2(0.5f, 0.5f),
                                       0.0f,
                                       Ogre::ColourValue::Red,
                                       1.0f, 0.5f);
    EXPECT_EQ(painted, 0);
}

TEST(TexturePaintBufferTest, PaintBrushClampsToBufferBounds)
{
    TexturePaintBuffer buf(8, 8);
    // Brush centered at UV (0, 1) — top-left corner pixel.
    const int painted = buf.paintBrush(Ogre::Vector2(0.0f, 1.0f),
                                       0.5f,
                                       Ogre::ColourValue::Red,
                                       1.0f, 0.0f);
    EXPECT_GT(painted, 0);
    // The dirty rect must lie within the buffer.
    const auto& d = buf.dirtyRect();
    EXPECT_GE(d.x0, 0);
    EXPECT_GE(d.y0, 0);
    EXPECT_LE(d.x1, 8);
    EXPECT_LE(d.y1, 8);
}

TEST(TexturePaintBufferTest, UvToPixelRoundTrip)
{
    TexturePaintBuffer buf(64, 32);
    int x = 0, y = 0;
    buf.uvToPixel(Ogre::Vector2(0.5f, 0.5f), x, y);
    EXPECT_EQ(x, 32);
    EXPECT_EQ(y, 16);
    buf.uvToPixel(Ogre::Vector2(0.0f, 0.0f), x, y);
    EXPECT_EQ(x, 0);
    EXPECT_EQ(y, 32); // V flipped
    buf.uvToPixel(Ogre::Vector2(1.0f, 1.0f), x, y);
    // 1.0 * 64 = 64 (out of bounds upper) — the helper doesn't clamp; that's
    // the consumer's job. Verify the computed value rather than asserting
    // in-bounds.
    EXPECT_EQ(x, 64);
    EXPECT_EQ(y, 0);
}

TEST(TexturePaintBufferTest, SaveAndLoadRoundTripPreservesPixels)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    TexturePaintBuffer buf(8, 8);
    buf.paintBrush(Ogre::Vector2(0.5f, 0.5f), 0.3f, Ogre::ColourValue(0.2f, 0.4f, 0.6f, 1.0f), 1.0f, 0.0f);
    const std::string path = dir.filePath("paint.png").toStdString();
    ASSERT_TRUE(buf.save(path));

    TexturePaintBuffer reloaded;
    ASSERT_TRUE(reloaded.load(path));
    EXPECT_EQ(reloaded.width(), 8);
    EXPECT_EQ(reloaded.height(), 8);
    // Center pixel should match (small tolerance for PNG byte rounding).
    int cx = 0, cy = 0;
    reloaded.uvToPixel(Ogre::Vector2(0.5f, 0.5f), cx, cy);
    EXPECT_NEAR(reloaded.pixel(cx, cy).r, 0.2f, 0.02f);
    EXPECT_NEAR(reloaded.pixel(cx, cy).g, 0.4f, 0.02f);
    EXPECT_NEAR(reloaded.pixel(cx, cy).b, 0.6f, 0.02f);
}

TEST(TexturePaintBufferTest, LoadOnNonExistentFileFails)
{
    TexturePaintBuffer buf;
    EXPECT_FALSE(buf.load("/definitely/does/not/exist/asdf.png"));
}

TEST(TexturePaintBufferTest, MarkDirtyExpandsRectExternally)
{
    TexturePaintBuffer buf(16, 16);
    EXPECT_TRUE(buf.dirtyRect().empty());
    buf.markDirty(3, 4, 7, 9);
    EXPECT_EQ(buf.dirtyRect().x0, 3);
    EXPECT_EQ(buf.dirtyRect().y0, 4);
    EXPECT_EQ(buf.dirtyRect().x1, 7);
    EXPECT_EQ(buf.dirtyRect().y1, 9);
    // Expand again — should grow.
    buf.markDirty(0, 0, 5, 5);
    EXPECT_EQ(buf.dirtyRect().x0, 0);
    EXPECT_EQ(buf.dirtyRect().y0, 0);
    EXPECT_EQ(buf.dirtyRect().x1, 7);
    EXPECT_EQ(buf.dirtyRect().y1, 9);
}
