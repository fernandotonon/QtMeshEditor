#include "TextureDecoder.h"
#include "PsxVramColor.h"
#include "VramSnapshot.h"

#include <gtest/gtest.h>

static void writeClut4(VramSnapshot &vram, int clutX, int clutY)
{
    for (int i = 0; i < 16; ++i) {
        const uint8_t v = static_cast<uint8_t>(i * 16);
        vram.setPixel(clutX + i, clutY, PsxVramColor::rgbaToBgr555(v, v, v, 255));
    }
}

static void writeClut8(VramSnapshot &vram, int clutX, int clutY)
{
    for (int i = 0; i < 256; ++i) {
        const uint8_t v = static_cast<uint8_t>(i);
        vram.setPixel(clutX + i, clutY, PsxVramColor::rgbaToBgr555(v, 128, 255 - v, 255));
    }
}

TEST(TextureDecoderTest, Decodes15bppTile)
{
    VramSnapshot vram;
    vram.setPixel(0, 0, PsxVramColor::rgbaToBgr555(0, 0, 255, 255));
    TextureDecoder decoder;
    TextureDecoder::MaterialKey key{};
    key.tpage = 0;
    key.bitDepth = TextureDecoder::BitDepth::Bpp15;
    const QRect region(0, 0, 1, 1);
    const QImage tile = decoder.decodeMaterial(vram, key, region);
    ASSERT_FALSE(tile.isNull());
    EXPECT_EQ(qAlpha(tile.pixel(0, 0)), 255);
    EXPECT_EQ(decoder.stats().decodedMaterials, 1);
    EXPECT_EQ(decoder.stats().cacheHits, 0);

    EXPECT_FALSE(decoder.cachedMaterial(key).isNull());
    const QImage cached = decoder.decodeMaterial(vram, key, region);
    EXPECT_FALSE(cached.isNull());
    EXPECT_EQ(decoder.stats().cacheHits, 1);
}

TEST(TextureDecoderTest, Decodes4bppWithClut)
{
    VramSnapshot vram;
    writeClut4(vram, 0, 480);
    vram.setPixel(0, 0, 0x000F);
    TextureDecoder decoder;
    TextureDecoder::MaterialKey key{};
    key.tpage = 0;
    key.clutX = 0;
    key.clutY = 480;
    key.bitDepth = TextureDecoder::BitDepth::Bpp4;
    const QImage tile = decoder.decodeMaterial(vram, key, QRect(0, 0, 1, 1));
    ASSERT_FALSE(tile.isNull());
    const QRgb px = tile.pixel(0, 0);
    EXPECT_GT(qRed(px), 200);
}

TEST(TextureDecoderTest, Decodes8bppWithClut)
{
    VramSnapshot vram;
    writeClut8(vram, 64, 480);
    constexpr uint16_t tpage8 = 0x80; // (tpage >> 7) & 3 == 1 → 8bpp
    const QRect page = VramSnapshot::tpageRect(tpage8);
    vram.setPixel(page.x(), page.y(), static_cast<uint16_t>(0x00FF));

    TextureDecoder decoder;
    TextureDecoder::MaterialKey key{};
    key.tpage = tpage8;
    key.clutX = 64;
    key.clutY = 480;
    key.bitDepth = TextureDecoder::BitDepth::Bpp8;
    const QImage tile = decoder.decodeMaterial(vram, key, QRect(0, 0, 1, 1));
    ASSERT_FALSE(tile.isNull());
    EXPECT_GT(qGreen(tile.pixel(0, 0)), 100);
}

TEST(TextureDecoderTest, MaterialKeyDedupesAcrossRegions)
{
    VramSnapshot vram;
    vram.setPixel(0, 0, PsxVramColor::rgbaToBgr555(255, 0, 0, 255));
    vram.setPixel(1, 0, PsxVramColor::rgbaToBgr555(0, 255, 0, 255));
    TextureDecoder decoder;
    TextureDecoder::MaterialKey key{};
    key.tpage = 0;
    key.bitDepth = TextureDecoder::BitDepth::Bpp15;

    const QImage a = decoder.decodeMaterial(vram, key, QRect(0, 0, 1, 1));
    const QImage b = decoder.decodeMaterial(vram, key, QRect(1, 0, 1, 1));
    ASSERT_FALSE(a.isNull());
    ASSERT_FALSE(b.isNull());
    EXPECT_NE(a.pixel(0, 0), b.pixel(0, 0));
    EXPECT_EQ(decoder.stats().decodedMaterials, 2);
    EXPECT_EQ(decoder.stats().cacheHits, 0);

    const QImage merged = decoder.decodeMaterial(vram, key, QRect(0, 0, 2, 1));
    ASSERT_FALSE(merged.isNull());
    EXPECT_EQ(merged.width(), 2);
    EXPECT_EQ(decoder.stats().cacheHits, 1);
}

TEST(TextureDecoderTest, StpBitSetsSemiTransparentAlpha)
{
    VramSnapshot vram;
    vram.setPixel(0, 0, PsxVramColor::rgbaToBgr555(255, 0, 0, 127));
    TextureDecoder decoder;
    TextureDecoder::MaterialKey key{};
    key.tpage = 0;
    key.bitDepth = TextureDecoder::BitDepth::Bpp15;
    key.drawModeBits = 1u << 11;
    const QImage tile = decoder.decodeMaterial(vram, key, QRect(0, 0, 1, 1));
    ASSERT_FALSE(tile.isNull());
    EXPECT_EQ(qAlpha(tile.pixel(0, 0)), 128);
}

TEST(TextureDecoderTest, DrawModeZeroMasksTransparentBlack)
{
    VramSnapshot vram;
    vram.setPixel(0, 0, 0);
    TextureDecoder decoder;
    TextureDecoder::MaterialKey opaqueBlack{};
    opaqueBlack.tpage = 0;
    opaqueBlack.bitDepth = TextureDecoder::BitDepth::Bpp15;
    opaqueBlack.drawModeBits = 1u << 11;

    TextureDecoder::MaterialKey transparentBlack = opaqueBlack;
    transparentBlack.drawModeBits = 0;

    const QImage opaque =
        decoder.decodeMaterial(vram, opaqueBlack, QRect(0, 0, 1, 1));
    const QImage transparent =
        decoder.decodeMaterial(vram, transparentBlack, QRect(0, 0, 1, 1));
    ASSERT_FALSE(opaque.isNull());
    ASSERT_FALSE(transparent.isNull());
    EXPECT_EQ(qAlpha(opaque.pixel(0, 0)), 255);
    EXPECT_EQ(qAlpha(transparent.pixel(0, 0)), 0);
}

TEST(TextureDecoderTest, WarnsOnOutOfRangeVramRead)
{
    VramSnapshot vram;
    TextureDecoder decoder;
    TextureDecoder::MaterialKey key{};
    // TPAGE at x=960; page-local (254,0,4,1) reads VRAM x=1214..1217 (>= 1024).
    key.tpage = 0x000F;
    key.bitDepth = TextureDecoder::BitDepth::Bpp15;
    const QImage tile = decoder.decodeMaterial(vram, key, QRect(254, 0, 4, 1));
    EXPECT_FALSE(tile.isNull());
    EXPECT_FALSE(decoder.warnings().isEmpty());
}

TEST(TextureDecoderTest, MaterialKeySeparatesSemiTransMode)
{
    TextureDecoder::MaterialKey a{};
    TextureDecoder::MaterialKey b{};
    a.semiTrans = 0;
    b.semiTrans = 1;
    EXPECT_TRUE(a.semiTrans != b.semiTrans);
    EXPECT_NE(qHash(a), qHash(b));
}
