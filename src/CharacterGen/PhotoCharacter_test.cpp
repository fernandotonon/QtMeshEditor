#include <gtest/gtest.h>

#include "PhotoCharacter.h"

using namespace CharacterGen;

namespace {
PhotoCharacter::Result sample()
{
    PhotoCharacter::Result r;
    r.ok = true;
    r.bodyShape = {1.0f, 2.0f};
    r.proportions.legs = 0.9f;
    r.haveSkin = true;        r.skin = {0.1f, 0.2f, 0.3f};
    r.haveEye = true;         r.eye = {0.4f, 0.5f, 0.6f};
    r.haveHairColour = true;  r.hairColour = {0.7f, 0.1f, 0.1f};
    r.haveHairStyle = true;   r.hairStyle = QStringLiteral("proc:bob");
    r.hairProcedural = Hair::presetParams(QStringLiteral("bob"));
    r.haveClothes = true;     r.topStyle = QStringLiteral("tshirt"); r.bottomStyle = QStringLiteral("pants");
    r.haveTopColour = true;   r.topColour = {0.9f, 0.9f, 0.1f};
    r.haveBottomColour = true; r.bottomColour = {0.1f, 0.1f, 0.9f};
    return r;
}
}  // namespace

TEST(PhotoCharacter, ApplyCopiesEverythingTheCallerDidNotChoose)
{
    Params p;
    PhotoCharacter::apply(sample(), p);
    EXPECT_EQ(p.bodyShape, (std::vector<float>{1.0f, 2.0f}));
    EXPECT_FLOAT_EQ(p.proportions.legs, 0.9f);
    EXPECT_EQ(p.hairStyle, QStringLiteral("proc:bob"));
    EXPECT_EQ(p.topStyle, QStringLiteral("tshirt"));
    EXPECT_FLOAT_EQ(p.skin[0], 0.1f);
    EXPECT_FLOAT_EQ(p.eye[2], 0.6f);
    EXPECT_FLOAT_EQ(p.hairColour[0], 0.7f);
    EXPECT_FLOAT_EQ(p.topColour[2], 0.1f);
}

TEST(PhotoCharacter, ExplicitChoicesWinOverThePhoto)
{
    Params p;
    p.skin = {0.8f, 0.6f, 0.5f};
    p.hairColour = {0.0f, 0.0f, 0.0f};
    p.hairStyle = QStringLiteral("short01");
    p.topStyle = QStringLiteral("tank");
    p.topColour = {1.0f, 1.0f, 1.0f};
    p.proportions.legs = 1.1f;
    PhotoCharacter::Keep keep;
    keep.skin = keep.hairColour = keep.hairStyle = keep.clothes = keep.proportions = true;
    PhotoCharacter::apply(sample(), p, keep);
    EXPECT_FLOAT_EQ(p.skin[0], 0.8f);
    EXPECT_FLOAT_EQ(p.hairColour[0], 0.0f);
    EXPECT_EQ(p.hairStyle, QStringLiteral("short01"));
    EXPECT_EQ(p.topStyle, QStringLiteral("tank"));
    EXPECT_FLOAT_EQ(p.topColour[0], 1.0f);
    EXPECT_FLOAT_EQ(p.proportions.legs, 1.1f);
    EXPECT_FLOAT_EQ(p.eye[2], 0.6f);                 // the eye was not chosen: the photo's iris is used
    EXPECT_EQ(p.bodyShape.size(), 2u);               // the body always comes from the photo
}

TEST(PhotoCharacter, ColourKeepsAreIndependentOfTheClothingStyles)
{
    Params p;
    p.topColour = {1.0f, 1.0f, 1.0f};
    PhotoCharacter::Keep keep;
    keep.topColour = true;   // the style may come from the photo, the colour was chosen
    PhotoCharacter::apply(sample(), p, keep);
    EXPECT_EQ(p.topStyle, QStringLiteral("tshirt"));
    EXPECT_FLOAT_EQ(p.topColour[0], 1.0f);
    EXPECT_FLOAT_EQ(p.bottomColour[2], 0.9f);
}

TEST(PhotoCharacter, EstimateRefusesAnUnreadableImage)
{
    Assets a;
    const auto r = PhotoCharacter::estimate(QImage(), a, PhotoCharacter::Options());
    EXPECT_FALSE(r.ok);
    EXPECT_FALSE(r.error.isEmpty());
}
