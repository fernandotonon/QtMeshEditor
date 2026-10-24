#include <gtest/gtest.h>

#include "HairTint.h"

#include <QBuffer>

using namespace CharacterGen;

namespace {
// A shading map: a horizontal ramp of greys, left half opaque, right half transparent.
QImage ramp()
{
    QImage img(64, 8, QImage::Format_ARGB32);
    for (int y = 0; y < img.height(); ++y)
        for (int x = 0; x < img.width(); ++x) {
            const int g = x * 255 / 31;
            img.setPixel(x, y, x < 32 ? qRgba(g, g, g, 255) : qRgba(200, 200, 200, 0));
        }
    return img;
}
float lum(QRgb p) { return 0.299f * qRed(p) + 0.587f * qGreen(p) + 0.114f * qBlue(p); }
}  // namespace

TEST(HairTint, PresetsAndParsing)
{
    ASSERT_EQ(hairColourPresets().size(), size_t(9));
    Rgb c{};
    EXPECT_TRUE(parseHairColour("Dark Brown", &c));
    EXPECT_EQ(hairColourHex(c), QStringLiteral("#32221a"));
    EXPECT_TRUE(parseHairColour("platinum-blonde", &c));
    EXPECT_TRUE(parseHairColour("#ff8000", &c));
    EXPECT_EQ(hairColourHex(c), QStringLiteral("#ff8000"));
    const Rgb before = c;
    EXPECT_FALSE(parseHairColour("purple", &c));
    EXPECT_FALSE(parseHairColour("#12345", &c));
    EXPECT_FALSE(parseHairColour("#gggggg", &c));
    EXPECT_EQ(c, before) << "a failed parse leaves the output alone";
}

TEST(HairTint, KeepsAlphaAndMapsTheMedianToTheColour)
{
    const Rgb colour{0.6f, 0.3f, 0.1f};
    const QImage out = tintHairShading(ramp(), colour);
    ASSERT_EQ(out.size(), QSize(64, 8));
    for (int x = 0; x < 64; ++x) EXPECT_EQ(qAlpha(out.pixel(x, 3)), x < 32 ? 255 : 0) << x;
    // the median opaque pixel (x ~ 16) is the mid stop: the colour itself
    const QRgb mid = out.pixel(16, 3);
    EXPECT_NEAR(qRed(mid), 0.6f * 255, 12);
    EXPECT_NEAR(qGreen(mid), 0.3f * 255, 12);
    EXPECT_NEAR(qBlue(mid), 0.1f * 255, 12);
    // darker towards the left, lighter towards the right of the opaque part
    EXPECT_LT(lum(out.pixel(1, 3)), lum(mid));
    EXPECT_GT(lum(out.pixel(31, 3)), lum(mid));
}

TEST(HairTint, DifferentColoursGiveDifferentPixelsAndNullStaysNull)
{
    const QImage a = tintHairShading(ramp(), {0.1f, 0.1f, 0.1f});
    const QImage b = tintHairShading(ramp(), {0.9f, 0.8f, 0.6f});
    EXPECT_LT(lum(a.pixel(16, 0)), lum(b.pixel(16, 0)));
    EXPECT_TRUE(tintHairShading(QImage(), {0.5f, 0.5f, 0.5f}).isNull());
}

TEST(HairTint, PngRoundTripAndBadInput)
{
    QByteArray png;
    QBuffer buf(&png);
    buf.open(QIODevice::WriteOnly);
    ASSERT_TRUE(ramp().save(&buf, "PNG"));
    const QByteArray out = tintHairTexturePng(png, {0.5f, 0.4f, 0.3f});
    ASSERT_FALSE(out.isEmpty());
    QImage back;
    ASSERT_TRUE(back.loadFromData(out, "PNG"));
    EXPECT_EQ(back.size(), QSize(64, 8));
    EXPECT_TRUE(tintHairTexturePng(QByteArray("not a png"), {0.5f, 0.4f, 0.3f}).isEmpty());
}

TEST(HairTint, AMapThatStillHasItsOwnColourIsNormalisedOnItsMedianLuminance)
{
    // an old-bundle texture: dark brown (luminance ~40). Tinting must still land the median on the requested colour, not on a black.
    QImage dark(16, 16, QImage::Format_ARGB32);
    dark.fill(qRgba(60, 35, 25, 255));
    const QImage out = tintHairShading(dark, {0.8f, 0.7f, 0.5f});
    EXPECT_NEAR(qRed(out.pixel(8, 8)), 0.8f * 255, 14);
}
