#include <gtest/gtest.h>

#include "UVTransform.h"

TEST(UVTransformTest, MedianPivotAtSelectionCenter)
{
    const std::vector<UVTransform::VertRef> verts = {
        {0, {0.f, 0.f}},
        {1, {1.f, 0.f}},
        {2, {0.f, 1.f}},
        {3, {1.f, 1.f}},
    };
    const Ogre::Vector2 pivot = UVTransform::medianPivot(verts);
    EXPECT_NEAR(pivot.x, 0.5f, 1e-5f);
    EXPECT_NEAR(pivot.y, 0.5f, 1e-5f);
}

TEST(UVTransformTest, MoveUsesMedianPivotByDefault)
{
    UVTransform::Settings settings;
    settings.pivot = UVTransform::PivotMode::Median;
    const std::vector<UVTransform::VertRef> verts = {
        {0, {0.f, 0.f}},
        {1, {1.f, 0.f}},
        {2, {0.f, 1.f}},
        {3, {1.f, 1.f}},
    };
    const auto out = UVTransform::applyTransform(
        UVTransform::TransformOp::Move, verts, settings, verts,
        {0.1f, 0.2f}, 0.f, false);
    ASSERT_EQ(out.size(), 4u);
    EXPECT_NEAR(out[0].uv.x, 0.1f, 1e-5f);
    EXPECT_NEAR(out[0].uv.y, 0.2f, 1e-5f);
    EXPECT_NEAR(out[3].uv.x, 1.1f, 1e-5f);
    EXPECT_NEAR(out[3].uv.y, 1.2f, 1e-5f);
}

TEST(UVTransformTest, IndividualOriginsRotateDiffersFromMedian)
{
    UVTransform::Settings medianSettings;
    medianSettings.pivot = UVTransform::PivotMode::Median;

    UVTransform::Settings individualSettings;
    individualSettings.pivot = UVTransform::PivotMode::IndividualOrigins;

    const std::vector<UVTransform::VertRef> verts = {
        {0, {0.f, 0.f}},
        {1, {1.f, 0.f}},
    };
    const auto medianOut = UVTransform::applyTransform(
        UVTransform::TransformOp::Rotate, verts, medianSettings, verts,
        Ogre::Vector2::ZERO, 90.f, true);
    const auto individualOut = UVTransform::applyTransform(
        UVTransform::TransformOp::Rotate, verts, individualSettings, verts,
        Ogre::Vector2::ZERO, 90.f, true);
    ASSERT_EQ(medianOut.size(), 2u);
    ASSERT_EQ(individualOut.size(), 2u);
    EXPECT_NEAR(medianOut[0].uv.x, 0.5f, 1e-4f);
    EXPECT_NEAR(medianOut[0].uv.y, -0.5f, 1e-4f);
    EXPECT_NEAR(medianOut[1].uv.x, 0.5f, 1e-4f);
    EXPECT_NEAR(medianOut[1].uv.y, 0.5f, 1e-4f);
    EXPECT_NEAR(individualOut[0].uv.x, 0.f, 1e-4f);
    EXPECT_NEAR(individualOut[1].uv.x, 1.f, 1e-4f);
}

TEST(UVTransformTest, CursorPivotRotate90)
{
    UVTransform::Settings settings;
    settings.pivot = UVTransform::PivotMode::Cursor;
    settings.cursor = {0.5f, 0.5f};
    const std::vector<UVTransform::VertRef> verts = {
        {0, {1.f, 0.5f}},
    };
    const auto out = UVTransform::applyTransform(
        UVTransform::TransformOp::Rotate, verts, settings, verts,
        Ogre::Vector2::ZERO, 90.f, true);
    ASSERT_EQ(out.size(), 1u);
    EXPECT_NEAR(out[0].uv.x, 0.5f, 1e-4f);
    EXPECT_NEAR(out[0].uv.y, 1.f, 1e-4f);
}

TEST(UVTransformTest, SnapToGrid)
{
    UVTransform::Settings settings;
    settings.snapEnabled = true;
    settings.gridSize = 0.25f;
    const Ogre::Vector2 snapped = UVTransform::snapUv({0.13f, 0.38f}, settings, {}, {});
    EXPECT_NEAR(snapped.x, 0.25f, 1e-5f);
    EXPECT_NEAR(snapped.y, 0.5f, 1e-5f);
}

TEST(UVTransformTest, MirrorXReversible)
{
    UVTransform::Settings settings;
    settings.pivot = UVTransform::PivotMode::Median;
    const std::vector<UVTransform::VertRef> verts = {
        {0, {0.2f, 0.3f}},
        {1, {0.8f, 0.7f}},
    };
    const auto mirrored = UVTransform::applyTransform(
        UVTransform::TransformOp::MirrorX, verts, settings, verts,
        Ogre::Vector2::ZERO, 0.f, false);
    const auto restored = UVTransform::applyTransform(
        UVTransform::TransformOp::MirrorX, mirrored, settings, mirrored,
        Ogre::Vector2::ZERO, 0.f, false);
    ASSERT_EQ(restored.size(), 2u);
    EXPECT_NEAR(restored[0].uv.x, verts[0].uv.x, 1e-5f);
    EXPECT_NEAR(restored[0].uv.y, verts[0].uv.y, 1e-5f);
    EXPECT_NEAR(restored[1].uv.x, verts[1].uv.x, 1e-5f);
    EXPECT_NEAR(restored[1].uv.y, verts[1].uv.y, 1e-5f);
}

TEST(UVTransformTest, NumericScaleHalf)
{
    UVTransform::Settings settings;
    const std::vector<UVTransform::VertRef> verts = {
        {0, {0.f, 0.f}},
        {1, {1.f, 1.f}},
    };
    const auto out = UVTransform::applyTransform(
        UVTransform::TransformOp::Scale, verts, settings, verts,
        Ogre::Vector2::ZERO, 0.5f, true);
    ASSERT_EQ(out.size(), 2u);
    EXPECT_NEAR(out[1].uv.x, 0.75f, 1e-5f);
    EXPECT_NEAR(out[1].uv.y, 0.75f, 1e-5f);
}
