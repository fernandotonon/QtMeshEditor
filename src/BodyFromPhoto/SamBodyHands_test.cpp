#include <gtest/gtest.h>

#include "SamBodyHands.h"

#include <cmath>

using namespace SamBody;
using namespace SamBody::Hands;

namespace {

// A 1000x1255 photo with the body box Meta's own test case uses (crop edge 1506.25 px, centre 435, 622.5).
CropGeometry bodyGeo()
{
    CropGeometry g;
    g.valid = true;
    g.photoW = 1000; g.photoH = 1255;
    g.center = {435.0, 622.5};
    g.scale = 1506.25;
    const double a = kCropSize / g.scale;
    g.affine = {float(a), 0.0f, float(256.0 - a * 435.0), 0.0f, float(a), float(256.0 - a * 622.5)};
    return g;
}

} // namespace

TEST(SamBodyHands, HandBoxMapsTheCropBoxBackToASquarePhotoBox)
{
    const CropGeometry g = bodyGeo();
    // a box centred in the crop, 0.1 x 0.05 of it: edge = max(w, h) = 0.1 of the crop = 150.625 photo px
    const QRectF b = handBoxInPhoto(g, {0.5f, 0.5f, 0.1f, 0.05f});
    EXPECT_NEAR(b.center().x(), 435.0, 1e-3);
    EXPECT_NEAR(b.center().y(), 622.5, 1e-3);
    EXPECT_NEAR(b.width(), 150.625, 1e-3);
    EXPECT_NEAR(b.height(), b.width(), 1e-9);
    EXPECT_TRUE(handBoxInPhoto(g, {NAN, 0.5f, 0.1f, 0.1f}).isEmpty());
    EXPECT_TRUE(handBoxInPhoto(g, {0.5f, 0.5f, 0.0f, 0.0f}).isEmpty());
}

TEST(SamBodyHands, MirrorFollowsMetasPixelConvention)
{
    // x1' = W - x2 - 1, x2' = W - x1 - 1
    const QRectF m = mirrorBox(QRectF(100, 50, 40, 40), 1000);
    EXPECT_DOUBLE_EQ(m.left(), 1000 - 140 - 1);
    EXPECT_DOUBLE_EQ(m.right(), 1000 - 100 - 1);
    EXPECT_DOUBLE_EQ(m.top(), 50);
    const QPointF p = mirrorPoint(QPointF(10, 20), 1000);
    EXPECT_DOUBLE_EQ(p.x(), 989);
    EXPECT_DOUBLE_EQ(mirrorPoint(p, 1000).x(), 10);   // involution
}

TEST(SamBodyHands, CropNormalisationIsCentredOnTheBox)
{
    const CropGeometry g = bodyGeo();
    const QPointF c = photoToCrop(g, QPointF(435.0, 622.5));
    EXPECT_NEAR(c.x(), 0.0, 1e-6);
    EXPECT_NEAR(c.y(), 0.0, 1e-6);
    const QPointF e = photoToCrop(g, QPointF(435.0 + 1506.25 / 2, 622.5));
    EXPECT_NEAR(e.x(), 0.5, 1e-6);
}

TEST(SamBodyHands, KeypointTestsUseMetasThresholds)
{
    EXPECT_TRUE(keypointsInside({0.1f, -0.49f, 0.0f, 0.2f}));
    EXPECT_FALSE(keypointsInside({0.1f, 0.5f}));          // strict
    EXPECT_FALSE(keypointsInside({}));
    EXPECT_FALSE(keypointsInside({NAN, 0.0f}));
    EXPECT_TRUE(wristClose(QPointF(0, 0), QPointF(24, 0), 100.0));    // 0.24 < 0.25
    EXPECT_FALSE(wristClose(QPointF(0, 0), QPointF(25, 0), 100.0));
    EXPECT_FALSE(wristClose(QPointF(0, 0), QPointF(1, 0), 0.0));
    EXPECT_TRUE(std::isnan(keypoint({1.0f, 2.0f}, 1).x()));
    EXPECT_FLOAT_EQ(float(keypoint({1.0f, 2.0f, 3.0f, 4.0f}, 1).y()), 4.0f);
}

TEST(SamBodyHands, PromptsKeepOnlyAcceptedSidesInsideTheCropInMetasOrder)
{
    const CropGeometry g = bodyGeo();
    const QPointF in(435.0, 622.5);
    const QPointF outside(435.0 + 1506.25, 622.5);   // x = 1.5 in crop space
    // both hands accepted: right wrist, left wrist, right elbow, left elbow
    auto p = wristElbowPrompts(g, in, in, in, in, {true, true});
    ASSERT_EQ(p.size(), 4u);
    EXPECT_FLOAT_EQ(p[0].label, float(kRightWristKp));
    EXPECT_FLOAT_EQ(p[1].label, float(kLeftWristKp));
    EXPECT_FLOAT_EQ(p[2].label, float(kRightElbowKp));
    EXPECT_FLOAT_EQ(p[3].label, float(kLeftElbowKp));
    EXPECT_NEAR(p[0].x, 0.5f, 1e-6f);                 // shifted from [-0.5, 0.5] to [0, 1]
    // only the right hand accepted -> its wrist and elbow
    p = wristElbowPrompts(g, in, in, in, in, {false, true});
    ASSERT_EQ(p.size(), 2u);
    EXPECT_FLOAT_EQ(p[0].label, float(kRightWristKp));
    EXPECT_FLOAT_EQ(p[1].label, float(kRightElbowKp));
    // a point outside the body crop is dropped
    p = wristElbowPrompts(g, outside, in, in, in, {true, true});
    ASSERT_EQ(p.size(), 3u);
    EXPECT_FLOAT_EQ(p[0].label, float(kLeftWristKp));
    EXPECT_TRUE(wristElbowPrompts(g, in, in, in, in, {false, false}).empty());
}
