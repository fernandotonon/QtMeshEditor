#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTemporaryFile>

#include <array>
#include <cstdint>
#include <vector>

#include <OgreImage.h>
#include <OgrePixelFormat.h>

#include "PS1/PS1TIM.h"

namespace {

void appendU32(QByteArray& b, uint32_t v)
{
    b.append(char(v & 0xFF));
    b.append(char((v >> 8) & 0xFF));
    b.append(char((v >> 16) & 0xFF));
    b.append(char((v >> 24) & 0xFF));
}

void appendU16(QByteArray& b, uint16_t v)
{
    b.append(char(v & 0xFF));
    b.append(char((v >> 8) & 0xFF));
}

/** 16 bpp direct-colour TIM, no CLUT: one red pixel embedded in smallest valid image block layout. */
QByteArray makeMinimalTim16_RedPixelAtOrigin()
{
    QByteArray tim;
    appendU32(tim, 0x10u);
    appendU32(tim, 0x02u); // bppMode = 2, no CLUT
    const uint16_t imgWWords = 1;
    const uint16_t imgH = 1;
    const uint32_t imgPayloadBytes = 12u + uint32_t(imgWWords) * uint32_t(imgH) * 2u;
    appendU32(tim, imgPayloadBytes);
    appendU16(tim, 0);
    appendU16(tim, 0);
    appendU16(tim, imgWWords);
    appendU16(tim, imgH);
    // Pure red PSX BGR555: R=31 in bits 0-4 → 0x001F per loader's psxBgr555ToRgba.
    appendU16(tim, 0x001Fu);
    return tim;
}

/** 8bpp indexed: CLUT row 256 wide, single image row with two palette indices mapping to red/green. */
QByteArray makeMinimalTim8_IndexedStrip()
{
    QByteArray tim;
    appendU32(tim, 0x10u);
    appendU32(tim, 0x09u); // bppMode = 1, has CLUT (bit 3)

    // CLUT block: 256×1 colours (512 bytes payload).
    const uint32_t clutLen = 12u + 256u * 2u;
    appendU32(tim, clutLen);
    appendU16(tim, 0);
    appendU16(tim, 0);
    appendU16(tim, 256); // clut width in words — must be ≥256 for 8bpp
    appendU16(tim, 1);
    for (int i = 0; i < 256; ++i) {
        uint16_t c = 0;
        if (i == 11)
            c = 0x001Fu;                     // saturated red channel
        else if (i == 22)
            c = uint16_t(31u << 5);         // saturated green middle bits
        appendU16(tim, c);
    }

    // Image: 8 pixels × 1 row → wWords = 4 (two indices per word).
    const uint16_t iw = 4;
    const uint16_t ih = 1;
    const uint32_t imgLen = 12u + uint32_t(iw) * uint32_t(ih) * 2u;
    appendU32(tim, imgLen);
    appendU16(tim, 0);
    appendU16(tim, 0);
    appendU16(tim, iw);
    appendU16(tim, ih);
    // Pixel pairs: idx 11 && 22 repeated across the strip.
    for (int i = 0; i < 4; ++i)
        appendU16(tim, uint16_t((22u << 8) | 11u));
    return tim;
}

/** 4bpp indexed: 16-entry CLUT, 4-pixel-wide row (single word holds 4 nibbles). */
QByteArray makeMinimalTim4_IndexedQuad()
{
    QByteArray tim;
    appendU32(tim, 0x10u);
    appendU32(tim, 0x08u); // bppMode = 0, has CLUT

    const uint32_t clutLen = 12u + 16u * 2u;
    appendU32(tim, clutLen);
    appendU16(tim, 0);
    appendU16(tim, 0);
    appendU16(tim, 16); // clut words — must be ≥16 for 4bpp
    appendU16(tim, 1);
    for (int i = 0; i < 16; ++i) {
        uint16_t c = 0;
        if (i == 1)
            c = uint16_t(31u << 10);       // blue
        else if (i == 2)
            c = 0x001Fu;                   // red
        appendU16(tim, c);
    }

    const uint16_t iw = 1;
    const uint16_t ih = 1;
    const uint32_t imgLen = 12u + uint32_t(iw) * uint32_t(ih) * 2u;
    appendU32(tim, imgLen);
    appendU16(tim, 0);
    appendU16(tim, 0);
    appendU16(tim, iw);
    appendU16(tim, ih);
    // Nibbles low→high: 1, 2, 1, 2
    appendU16(tim, uint16_t(0x2121u));
    return tim;
}

bool writeFile(const QString& path, const QByteArray& data)
{
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly))
        return false;
    return f.write(data) == data.size();
}

} // namespace

TEST(PS1TIMTest, SaveThenLoadTim16RoundTrip)
{
    // 2x2 RGBA image with distinct colors.
    std::vector<uint8_t> rgba = {
        255, 0, 0, 255,      0, 255, 0, 255,
        0, 0, 255, 255,      255, 255, 255, 255
    };

    Ogre::Image img;
    img.loadDynamicImage(rgba.data(), 2, 2, 1, Ogre::PF_BYTE_RGBA, false);

    QTemporaryFile tmp(QDir::tempPath() + "/qtmesh_ps1tim_XXXXXX.tim");
    tmp.setAutoRemove(true);
    ASSERT_TRUE(tmp.open());
    const QString path = tmp.fileName();
    tmp.close();

    QString err;
    ASSERT_TRUE(PS1TIM::saveOgreImageToTim16(img, path, &err)) << err.toStdString();

    Ogre::Image decoded;
    ASSERT_TRUE(PS1TIM::loadTimToOgreImage(path, decoded, &err)) << err.toStdString();
    ASSERT_EQ(decoded.getFormat(), Ogre::PF_BYTE_RGBA);
    ASSERT_EQ(decoded.getWidth(), 256u);
    ASSERT_EQ(decoded.getHeight(), 256u);

    // Top-left should match (within 5-bit quantization).
    const uint8_t* d = decoded.getData();
    auto px = [&](int x, int y) {
        const size_t i = (size_t(y) * 256u + size_t(x)) * 4u;
        return std::array<uint8_t, 4>{d[i + 0], d[i + 1], d[i + 2], d[i + 3]};
    };
    const auto p00 = px(0, 0);
    EXPECT_GE(p00[0], 240);
    EXPECT_LE(p00[1], 16);
    EXPECT_LE(p00[2], 16);
    EXPECT_EQ(p00[3], 255);
}

TEST(PS1TIMTest, LoadFails_WhenFileMissing)
{
    Ogre::Image img;
    QString err;
    EXPECT_FALSE(PS1TIM::loadTimToOgreImage(QStringLiteral("/nonexistent/dir/model.tim"), img, &err));
    EXPECT_FALSE(err.isEmpty());
}

TEST(PS1TIMTest, LoadFails_WhenBadMagic)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = QDir(dir.path()).filePath(QStringLiteral("bad.tim"));
    ASSERT_TRUE(writeFile(path, QByteArray("NOPE")));

    Ogre::Image img;
    QString err;
    EXPECT_FALSE(PS1TIM::loadTimToOgreImage(path, img, &err));
    EXPECT_FALSE(err.isEmpty());
}

TEST(PS1TIMTest, LoadFails_WhenTooSmall)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = QDir(dir.path()).filePath(QStringLiteral("tiny.tim"));
    const QByteArray b(4, '\0');
    ASSERT_TRUE(writeFile(path, b));

    Ogre::Image img;
    QString err;
    EXPECT_FALSE(PS1TIM::loadTimToOgreImage(path, img, &err));
}

TEST(PS1TIMTest, LoadFails_WhenUnsupportedBppMode)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = QDir(dir.path()).filePath(QStringLiteral("mode3.tim"));
    QByteArray b;
    appendU32(b, 0x10u);
    appendU32(b, 0x03u); // bppMode 3 — unsupported
    ASSERT_TRUE(writeFile(path, b));

    Ogre::Image img;
    QString err;
    EXPECT_FALSE(PS1TIM::loadTimToOgreImage(path, img, &err));
}

TEST(PS1TIMTest, LoadFails_WhenIndexedWithoutClut)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = QDir(dir.path()).filePath(QStringLiteral("noclut.tim"));
    QByteArray b;
    appendU32(b, 0x10u);
    appendU32(b, 0x01u); // 8bpp but bit3 clear → missing CLUT
    ASSERT_TRUE(writeFile(path, b));

    Ogre::Image img;
    QString err;
    EXPECT_FALSE(PS1TIM::loadTimToOgreImage(path, img, &err));
}

TEST(PS1TIMTest, LoadFails_WhenTruncatedBlockHeader)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = QDir(dir.path()).filePath(QStringLiteral("trunc.tim"));
    QByteArray b;
    appendU32(b, 0x10u);
    appendU32(b, 0x02u);
    appendU32(b, 8u); // claims small block but only 8 bytes total file after this would need more
    ASSERT_TRUE(writeFile(path, b));

    Ogre::Image img;
    QString err;
    EXPECT_FALSE(PS1TIM::loadTimToOgreImage(path, img, &err));
}

TEST(PS1TIMTest, LoadFails_WhenImagePayloadTruncated)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = QDir(dir.path()).filePath(QStringLiteral("shortimg.tim"));
    QByteArray b;
    appendU32(b, 0x10u);
    appendU32(b, 0x02u);
    // Block length 20 (12 hdr + 8 bytes) but declare 2×2 image needs 4 words = 8 bytes — actually valid.
    // Instead: claim wWords=16, h=16 → 256 words = 512 bytes payload, provide only the header.
    appendU32(b, 12u + 4u); // lie: only 4 bytes of image data after header
    appendU16(b, 0);
    appendU16(b, 0);
    appendU16(b, 16);
    appendU16(b, 16);
    appendU16(b, 0); // one word only
    ASSERT_TRUE(writeFile(path, b));

    Ogre::Image img;
    QString err;
    EXPECT_FALSE(PS1TIM::loadTimToOgreImage(path, img, &err));
}

TEST(PS1TIMTest, LoadDecodes16bppDirectImage)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = QDir(dir.path()).filePath(QStringLiteral("red.tim"));
    ASSERT_TRUE(writeFile(path, makeMinimalTim16_RedPixelAtOrigin()));

    Ogre::Image img;
    QString err;
    ASSERT_TRUE(PS1TIM::loadTimToOgreImage(path, img, &err)) << err.toStdString();
    const uint8_t* d = img.getData();
    const std::array<uint8_t, 4> p0{d[0], d[1], d[2], d[3]};
    EXPECT_GE(p0[0], 240);
    EXPECT_LE(p0[1], 16);
    EXPECT_LE(p0[2], 16);
    EXPECT_EQ(p0[3], 255);
}

TEST(PS1TIMTest, LoadDecodes8bppIndexedImage)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = QDir(dir.path()).filePath(QStringLiteral("i8.tim"));
    ASSERT_TRUE(writeFile(path, makeMinimalTim8_IndexedStrip()));

    Ogre::Image img;
    QString err;
    ASSERT_TRUE(PS1TIM::loadTimToOgreImage(path, img, &err)) << err.toStdString();
    const uint8_t* d = img.getData();
    auto px = [&](int x, int y) {
        const size_t i = (size_t(y) * 256u + size_t(x)) * 4u;
        return std::array<uint8_t, 4>{d[i + 0], d[i + 1], d[i + 2], d[i + 3]};
    };
    // First column index 11 → red
    const auto a = px(0, 0);
    EXPECT_GE(a[0], 240);
    EXPECT_LE(a[1], 24);
}

TEST(PS1TIMTest, LoadDecodes4bppIndexedImage)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = QDir(dir.path()).filePath(QStringLiteral("i4.tim"));
    ASSERT_TRUE(writeFile(path, makeMinimalTim4_IndexedQuad()));

    Ogre::Image img;
    QString err;
    ASSERT_TRUE(PS1TIM::loadTimToOgreImage(path, img, &err)) << err.toStdString();
    const uint8_t* d = img.getData();
    const auto p0 = std::array<uint8_t, 4>{d[0], d[1], d[2], d[3]};
    EXPECT_LE(p0[0], 32);
    EXPECT_LE(p0[1], 32);
    EXPECT_GE(p0[2], 240);
}

TEST(PS1TIMTest, SaveFails_WhenOutputCannotBeOpenedForWrite)
{
    std::vector<uint8_t> rgba = {128, 128, 128, 255};
    Ogre::Image img;
    img.loadDynamicImage(rgba.data(), 1, 1, 1, Ogre::PF_BYTE_RGBA, false);

    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString nested = QDir(dir.path()).filePath(QStringLiteral("no_such_subdirectory/out.tim"));

    QString err;
    EXPECT_FALSE(PS1TIM::saveOgreImageToTim16(img, nested, &err));
    EXPECT_FALSE(err.isEmpty());
}

TEST(PS1TIMTest, SaveEncodesLowAlphaWithStpBit)
{
    std::vector<uint8_t> rgba = {255, 255, 255, 50};
    Ogre::Image img;
    img.loadDynamicImage(rgba.data(), 1, 1, 1, Ogre::PF_BYTE_RGBA, false);

    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = QDir(dir.path()).filePath(QStringLiteral("semi.tim"));
    QString err;
    ASSERT_TRUE(PS1TIM::saveOgreImageToTim16(img, path, &err)) << err.toStdString();

    QFile f(path);
    ASSERT_TRUE(f.open(QIODevice::ReadOnly));
    const QByteArray raw = f.readAll();
    ASSERT_GE(raw.size(), 22);
    // First BGR555 pixel word at offset 20 (after 8-byte file header + 12-byte image block header).
    const auto* bytes = reinterpret_cast<const uint8_t*>(raw.constData());
    const uint16_t px = uint16_t(bytes[20]) | (uint16_t(bytes[21]) << 8);
    EXPECT_NE(px & 0x8000u, 0u);
}
