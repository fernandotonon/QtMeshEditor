#include <gtest/gtest.h>

#include "Proportions.h"

using namespace CharacterGen;

namespace {
// a 4-joint chain standing on the floor: body_world(0,0) -> root(0,90) -> l_upleg(0,90 .. hip) -> l_lowleg (0,45); vertices: the foot, the knee, the hip, the head
Assets chain(std::vector<std::array<float, 3>>& gt, std::vector<float>& pos)
{
    Assets a;
    a.jointNames = QStringList{QStringLiteral("body_world"), QStringLiteral("root"), QStringLiteral("l_upleg"), QStringLiteral("l_lowleg"), QStringLiteral("c_head")};
    a.jointParents = {-1, 0, 1, 2, 1};
    gt = {{0, 0, 0}, {0, 90, 0}, {0, 90, 0}, {0, 45, 0}, {0, 150, 0}};
    pos = {0, 0, 0,  0, 45, 0,  0, 90, 0,  0, 160, 0};
    a.skinStart = {0, 1, 2, 3, 4};
    a.skinJoint = {3, 3, 2, 4};
    a.skinWeight = {1, 1, 1, 1};
    return a;
}
}  // namespace

TEST(Proportions, DefaultLeavesEverythingAlone)
{
    std::vector<std::array<float, 3>> gt; std::vector<float> pos;
    const Assets a = chain(gt, pos);
    const auto before = pos;
    const auto fr = applyProportions(a, Proportions(), gt, pos);
    EXPECT_FALSE(fr.active);
    EXPECT_EQ(pos, before);
}

TEST(Proportions, HeightScalesTheWholeBodyAndKeepsTheFeetOnTheFloor)
{
    std::vector<std::array<float, 3>> gt; std::vector<float> pos;
    const Assets a = chain(gt, pos);
    Proportions p; p.height = 0.9f;
    const auto fr = applyProportions(a, p, gt, pos);
    ASSERT_TRUE(fr.active);
    EXPECT_NEAR(pos[1], 0.0f, 1e-3f);              // the foot is still on the floor
    EXPECT_NEAR(pos[10], 160.0f * 0.9f, 1e-2f);    // the head top: 160 -> 144 cm
    EXPECT_NEAR(gt[1][1], 90.0f * 0.9f, 1e-2f);
}

TEST(Proportions, LegsOnlyMoveTheLegAboutTheHip)
{
    std::vector<std::array<float, 3>> gt; std::vector<float> pos;
    const Assets a = chain(gt, pos);
    Proportions p; p.legs = 0.8f;
    applyProportions(a, p, gt, pos);
    // the knee and foot move towards the hip; the floor re-grounds, so measure the leg length: hip - foot
    EXPECT_NEAR(pos[7] - pos[1], 90.0f * 0.8f, 1e-2f);   // hip vertex (index 2: y at 7) minus the foot vertex (y at 1)
    EXPECT_NEAR(gt[2][1] - gt[3][1], 45.0f * 0.8f, 1e-2f);   // the hip-to-knee bone is 20 % shorter
}

TEST(Proportions, TorsoLengthIsClampedAndTheOutOfRangeIsNotAPerson)
{
    Proportions p; p.height = 5.0f; p.legs = 0.1f;
    p.clamp();
    EXPECT_FLOAT_EQ(p.height, 1.6f);
    EXPECT_FLOAT_EQ(p.legs, 0.6f);
    EXPECT_FALSE(p.isDefault());
    EXPECT_TRUE(Proportions().isDefault());
}
