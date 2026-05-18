#include "TextureDecoder.h"
#include "PsxVramColor.h"
#include "VramSnapshot.h"

#include <gtest/gtest.h>

static void writeClut(VramSnapshot &vram, int clutX, int clutY)
{
    for (int i = 0; i < 16; ++i) {
        const uint8_t v = static_cast<uint8_t>(i * 16);
        vram.setPixel(clutX + i, clutY, PsxVramColor::rgbaToBgr555(v, v, v, 255));
    }
}

TEST(TextureDecoderTest, Decodes15bppTile)
{
    VramSnapshot vram;
    vram.setPixel(0, 0, PsxVramColor::rgbaToBgr555(0, 0, 255, 255));
    TextureDecoder decoder;
    TextureDecoder::TileKey key{};
    key.tpage = 0;
    key.bitDepth = TextureDecoder::BitDepth::Bpp15;
    const QRect region(0, 0, 1, 1);
    const QImage tile = decoder.decodeTile(vram, key, region);
    ASSERT_FALSE(tile.isNull());
    EXPECT_EQ(qAlpha(tile.pixel(0, 0)), 255);
    EXPECT_EQ(decoder.stats().decodedTiles, 1);
    EXPECT_EQ(decoder.stats().cacheHits, 0);

    key.regionOnPage = region;
    EXPECT_FALSE(decoder.cachedTile(key).isNull());
    const QImage cached = decoder.decodeTile(vram, key, region);
    EXPECT_FALSE(cached.isNull());
    EXPECT_EQ(decoder.stats().cacheHits, 1);
}

TEST(TextureDecoderTest, Decodes4bppWithClut)
{
    VramSnapshot vram;
    writeClut(vram, 0, 480);
    vram.setPixel(0, 0, 0x000F); // index 15 in low nibble
    TextureDecoder decoder;
    TextureDecoder::TileKey key{};
    key.tpage = 0;
    key.clutX = 0;
    key.clutY = 480;
    key.bitDepth = TextureDecoder::BitDepth::Bpp4;
    const QImage tile = decoder.decodeTile(vram, key, QRect(0, 0, 1, 1));
    ASSERT_FALSE(tile.isNull());
    const QRgb px = tile.pixel(0, 0);
    EXPECT_GT(qRed(px), 200);
}

TEST(TextureDecoderTest, CacheKeyIncludesRegion)
{
    VramSnapshot vram;
    vram.setPixel(0, 0, PsxVramColor::rgbaToBgr555(255, 0, 0, 255));
    vram.setPixel(1, 0, PsxVramColor::rgbaToBgr555(0, 255, 0, 255));
    TextureDecoder decoder;
    TextureDecoder::TileKey key{};
    key.tpage = 0;
    key.bitDepth = TextureDecoder::BitDepth::Bpp15;

    const QImage a = decoder.decodeTile(vram, key, QRect(0, 0, 1, 1));
    const QImage b = decoder.decodeTile(vram, key, QRect(1, 0, 1, 1));
    ASSERT_FALSE(a.isNull());
    ASSERT_FALSE(b.isNull());
    EXPECT_NE(a.pixel(0, 0), b.pixel(0, 0));
    EXPECT_EQ(decoder.stats().decodedTiles, 2);
}
