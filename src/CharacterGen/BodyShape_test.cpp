#include <gtest/gtest.h>

#include "BodyShape.h"

using namespace CharacterGen;

// a tiny synthetic "body": a torso box, two legs and a head, with two shape vectors (taller / wider waist)
namespace {
Assets tiny()
{
    Assets a;
    a.jointNames = QStringList{QStringLiteral("spine_01"), QStringLiteral("l_upleg"), QStringLiteral("head")};
    const float H = 170.0f;
    // vertices: 0 floor (leg), 1 crotch (torso), 2/3 waist left/right (torso), 4/5 chest l/r (torso), 6 top of the head (head), 7 chin (head)
    const float p[][3] = {{0, 0, 0}, {0, 0.47f * H, 0}, {-15, 0.62f * H, 0}, {15, 0.62f * H, 0}, {-18, 0.73f * H, 0}, {18, 0.73f * H, 0}, {0, H, 0}, {0, 0.87f * H, 0}};
    a.mhrVertices = 8;
    for (auto& v : p) a.mhrBase.insert(a.mhrBase.end(), v, v + 3);
    const int cls[8] = {1, 0, 0, 0, 0, 0, 2, 2};
    a.skinStart = {0, 1, 2, 3, 4, 5, 6, 7, 8};
    for (int v = 0; v < 8; ++v) { a.skinJoint.push_back(cls[v]); a.skinWeight.push_back(1.0f); }
    a.mhrShapes = 2;
    a.mhrShapeVectors.assign(2 * 8 * 3, 0.0f);
    // mode 0: taller (every y scaled by 1%), mode 1: waist 2 cm wider each side
    for (int v = 0; v < 8; ++v) a.mhrShapeVectors[size_t(v) * 3 + 1] = a.mhrBase[size_t(v) * 3 + 1] * 0.01f;
    a.mhrShapeVectors[24 + 2 * 3] = -2.0f;
    a.mhrShapeVectors[24 + 3 * 3] = 2.0f;
    return a;
}
}  // namespace

TEST(BodyShape, MeasuresAndLinearisesTheModes)
{
    const Assets a = tiny();
    BodyShape::Model m;
    ASSERT_TRUE(m.build(a));
    EXPECT_NEAR(m.mean()[BodyShape::Height], 170.0f, 1e-3f);
    EXPECT_NEAR(m.mean()[BodyShape::WaistWidth], 30.0f, 1e-3f);
    EXPECT_NEAR(m.effect(BodyShape::Height, 0), 1.7f, 1e-3f);
    EXPECT_NEAR(m.effect(BodyShape::WaistWidth, 1), 4.0f, 1e-3f);
    EXPECT_NEAR(m.effect(BodyShape::WaistWidth, 0), 0.0f, 1e-3f);
}

TEST(BodyShape, WithOffsetChangesOneMeasurementOnly)
{
    const Assets a = tiny();
    BodyShape::Model m;
    ASSERT_TRUE(m.build(a));
    std::vector<float> c(2, 0.0f);
    c = m.withOffset(BodyShape::WaistWidth, 6.0f, c);
    EXPECT_NEAR(m.offset(BodyShape::WaistWidth, c), 6.0f, 0.3f);
    EXPECT_NEAR(m.offset(BodyShape::Height, c), 0.0f, 0.3f);   // the other measurement stays where it was
    // the exact measurement agrees with the linear model for this (exactly linear) body
    EXPECT_NEAR(m.measure(c)[BodyShape::WaistWidth], 36.0f, 0.4f);
}

TEST(BodyShape, WithOffsetIsClampedAndInvalidModelIsInert)
{
    const Assets a = tiny();
    BodyShape::Model m;
    ASSERT_TRUE(m.build(a));
    const auto c = m.withOffset(BodyShape::Height, 1000.0f, {0.0f, 0.0f}, 3.0f);
    EXPECT_LE(c[0], 3.0f);
    BodyShape::Model none;
    EXPECT_FALSE(none.valid());
    EXPECT_EQ(none.withOffset(BodyShape::Height, 5.0f, {1.0f, 2.0f}).size(), 2u);
    EXPECT_EQ(none.offset(BodyShape::Height, {1.0f}), 0.0f);
}

TEST(BodyShape, FitToTheSameBodyChangesNothing)
{
    const Assets a = tiny();
    BodyShape::Model m;
    ASSERT_TRUE(m.build(a));
    const auto fit = BodyShape::fitToBody(m, a, a.mhrBase, {0.0f, 0.0f});
    ASSERT_TRUE(fit.ok);
    EXPECT_NEAR(fit.proportions.legs, 1.0f, 1e-3f);
    EXPECT_NEAR(fit.proportions.head, 1.0f, 1e-3f);
    EXPECT_NEAR(fit.coefficients[1], 0.0f, 0.05f);
}

TEST(BodyShape, FitToAWiderWaistUsesTheCoefficientsAndAShorterLegUsesTheProportions)
{
    const Assets a = tiny();
    BodyShape::Model m;
    ASSERT_TRUE(m.build(a));
    std::vector<float> target = a.mhrBase;
    target[2 * 3] -= 3.0f;   // waist left vertex 3 cm further out
    target[3 * 3] += 3.0f;
    for (size_t v : {size_t(1)}) target[v * 3 + 1] -= 8.0f;   // the crotch 8 cm lower = shorter legs
    const auto fit = BodyShape::fitToBody(m, a, target, {0.0f, 0.0f});
    ASSERT_TRUE(fit.ok);
    EXPECT_GT(fit.coefficients[1], 0.5f);          // mode 1 widens the waist
    EXPECT_LT(fit.proportions.legs, 0.95f);        // the legs got shorter
}

TEST(BodyShape, RmsDistance)
{
    EXPECT_FLOAT_EQ(BodyShape::rmsDistance({0, 0, 0, 3, 4, 0}, {0, 0, 0, 0, 0, 0}), std::sqrt(12.5f));
    EXPECT_LT(BodyShape::rmsDistance({0, 0, 0}, {0, 0, 0, 1, 1, 1}), 0.0f);
}

TEST(BodyShape, ABoundedModeIsPinnedAndTheOthersCompensate)
{
    const Assets a = tiny();
    BodyShape::Model m;
    ASSERT_TRUE(m.build(a));
    // waist +6 cm needs mode 1 at 1.5; with a limit of 1.0 the mode is pinned and the result still holds at its bound (no silent clip of another measurement)
    const auto c = m.withOffset(BodyShape::WaistWidth, 6.0f, {0.0f, 0.0f}, 1.0f);
    EXPECT_LE(std::fabs(c[1]), 1.0f + 1e-6f);
    EXPECT_NEAR(m.offset(BodyShape::Height, c), 0.0f, 0.5f);
}

TEST(BodyShape, MalformedSkinArraysAreRejected)
{
    Assets a = tiny();
    a.skinStart[3] = 99;   // beyond the joint / weight arrays
    BodyShape::Model m;
    EXPECT_FALSE(m.build(a));
}
