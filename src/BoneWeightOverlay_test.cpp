#include <gtest/gtest.h>
#include "BoneWeightOverlay.h"

// Test the weightToColor function which doesn't require Ogre initialization

TEST(BoneWeightOverlayTest, WeightToColorAtZeroIsBlue)
{
    auto color = BoneWeightOverlay::weightToColor(0.0f);
    EXPECT_NEAR(color.r, 0.0f, 1e-5f);
    EXPECT_NEAR(color.g, 0.0f, 1e-5f);
    EXPECT_NEAR(color.b, 1.0f, 1e-5f);
    EXPECT_NEAR(color.a, 0.7f, 1e-5f);
}

TEST(BoneWeightOverlayTest, WeightToColorAtQuarterIsCyan)
{
    auto color = BoneWeightOverlay::weightToColor(0.25f);
    EXPECT_NEAR(color.r, 0.0f, 1e-5f);
    EXPECT_NEAR(color.g, 1.0f, 1e-5f);
    EXPECT_NEAR(color.b, 1.0f, 1e-5f);
    EXPECT_NEAR(color.a, 0.7f, 1e-5f);
}

TEST(BoneWeightOverlayTest, WeightToColorAtHalfIsGreen)
{
    auto color = BoneWeightOverlay::weightToColor(0.5f);
    EXPECT_NEAR(color.r, 0.0f, 1e-5f);
    EXPECT_NEAR(color.g, 1.0f, 1e-5f);
    EXPECT_NEAR(color.b, 0.0f, 1e-5f);
    EXPECT_NEAR(color.a, 0.7f, 1e-5f);
}

TEST(BoneWeightOverlayTest, WeightToColorAtThreeQuartersIsYellow)
{
    auto color = BoneWeightOverlay::weightToColor(0.75f);
    EXPECT_NEAR(color.r, 1.0f, 1e-5f);
    EXPECT_NEAR(color.g, 1.0f, 1e-5f);
    EXPECT_NEAR(color.b, 0.0f, 1e-5f);
    EXPECT_NEAR(color.a, 0.7f, 1e-5f);
}

TEST(BoneWeightOverlayTest, WeightToColorAtOneIsRed)
{
    auto color = BoneWeightOverlay::weightToColor(1.0f);
    EXPECT_NEAR(color.r, 1.0f, 1e-5f);
    EXPECT_NEAR(color.g, 0.0f, 1e-5f);
    EXPECT_NEAR(color.b, 0.0f, 1e-5f);
    EXPECT_NEAR(color.a, 0.7f, 1e-5f);
}

TEST(BoneWeightOverlayTest, WeightToColorClampsNegative)
{
    auto color = BoneWeightOverlay::weightToColor(-0.5f);
    auto colorZero = BoneWeightOverlay::weightToColor(0.0f);
    EXPECT_NEAR(color.r, colorZero.r, 1e-5f);
    EXPECT_NEAR(color.g, colorZero.g, 1e-5f);
    EXPECT_NEAR(color.b, colorZero.b, 1e-5f);
}

TEST(BoneWeightOverlayTest, WeightToColorClampsAboveOne)
{
    auto color = BoneWeightOverlay::weightToColor(1.5f);
    auto colorOne = BoneWeightOverlay::weightToColor(1.0f);
    EXPECT_NEAR(color.r, colorOne.r, 1e-5f);
    EXPECT_NEAR(color.g, colorOne.g, 1e-5f);
    EXPECT_NEAR(color.b, colorOne.b, 1e-5f);
}

TEST(BoneWeightOverlayTest, WeightToColorMidpointsInterpolate)
{
    // At 0.125 (halfway between blue and cyan), green should be 0.5
    auto color = BoneWeightOverlay::weightToColor(0.125f);
    EXPECT_NEAR(color.r, 0.0f, 1e-5f);
    EXPECT_NEAR(color.g, 0.5f, 1e-5f);
    EXPECT_NEAR(color.b, 1.0f, 1e-5f);

    // At 0.625 (halfway between green and yellow), red should be 0.5
    auto color2 = BoneWeightOverlay::weightToColor(0.625f);
    EXPECT_NEAR(color2.r, 0.5f, 1e-5f);
    EXPECT_NEAR(color2.g, 1.0f, 1e-5f);
    EXPECT_NEAR(color2.b, 0.0f, 1e-5f);
}
