#include <gtest/gtest.h>

#include "SkinTexture.h"

using namespace CharacterGen;

namespace {

/// A unit quad (two triangles) in the xy plane, 1 m wide, covering the whole UV square.
Dna::Mesh quad()
{
    Dna::Mesh m;
    m.positions = {0, 0, 0,  1, 0, 0,  1, 1, 0,  0, 1, 0};
    m.uvs = {0, 0,  1, 0,  1, 1,  0, 1};
    for (uint32_t i = 0; i < 4; ++i) m.layouts.push_back({i, i, i});
    m.faces = {{0, 1, 2}, {0, 2, 3}};
    return m;
}

}  // namespace

TEST(SkinTexture, BakesTheWholeUvSquareAroundTheSkinColour)
{
    SkinTextureOptions o;
    o.size = 128;
    const Rgb skin{0.80f, 0.60f, 0.48f};
    const QImage img = bakeSkinAlbedo(quad(), skin, o);
    ASSERT_FALSE(img.isNull());
    EXPECT_EQ(img.width(), 128);
    double r = 0;
    double g = 0;
    double b = 0;
    for (int y = 0; y < 128; ++y)
        for (int x = 0; x < 128; ++x) {
            const QRgb c = img.pixel(x, y);
            r += qRed(c);
            g += qGreen(c);
            b += qBlue(c);
        }
    const double n = 128.0 * 128.0 * 255.0;
    EXPECT_NEAR(r / n, skin[0], 0.10);   // variation is subtle: the mean stays near the requested colour
    EXPECT_NEAR(g / n, skin[1], 0.10);
    EXPECT_NEAR(b / n, skin[2], 0.10);
}

TEST(SkinTexture, SameSeedSameSkinDifferentSeedDifferentSkin)
{
    SkinTextureOptions a;
    a.size = 64;
    a.seed = 5;
    SkinTextureOptions b = a;
    b.seed = 6;
    const Rgb skin{0.80f, 0.60f, 0.48f};
    EXPECT_EQ(bakeSkinAlbedo(quad(), skin, a), bakeSkinAlbedo(quad(), skin, a));
    EXPECT_NE(bakeSkinAlbedo(quad(), skin, a), bakeSkinAlbedo(quad(), skin, b));
}

TEST(SkinTexture, ColourIsContinuousAcrossSeamsBecauseItDependsOnThePositionOnly)
{
    SkinTextureOptions o;
    o.seed = 3;
    const Rgb skin{0.7f, 0.5f, 0.4f};
    const Rgb a = skinColourAt({0.1f, 0.2f, 0.3f}, skin, o);
    const Rgb b = skinColourAt({0.1f, 0.2f, 0.3f}, skin, o);
    EXPECT_EQ(a, b);
    // a point a micrometre away is practically the same colour (no UV involved, so no seam)
    const Rgb c = skinColourAt({0.1f + 1e-6f, 0.2f, 0.3f}, skin, o);
    EXPECT_NEAR(a[0], c[0], 0.01f);
}

TEST(SkinTexture, PoresCanBeSwitchedOffAndAMeshWithoutUvsGivesNoImage)
{
    SkinTextureOptions smooth;
    smooth.poreStrength = 0.0f;
    smooth.freckles = 0.0f;
    smooth.seed = 2;
    const Rgb skin{0.8f, 0.6f, 0.5f};
    // without pores the colour only varies at the large scale: two points 2 mm apart barely differ
    const Rgb a = skinColourAt({0.5f, 0.5f, 0.5f}, skin, smooth);
    const Rgb b = skinColourAt({0.502f, 0.5f, 0.5f}, skin, smooth);
    EXPECT_NEAR(a[0], b[0], 0.01f);
    Dna::Mesh none;
    none.positions = {0, 0, 0};
    EXPECT_TRUE(bakeSkinAlbedo(none, skin).isNull());
}
