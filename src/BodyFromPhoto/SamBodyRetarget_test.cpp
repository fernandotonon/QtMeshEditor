#include <gtest/gtest.h>

#include "SamBodyMotion.h"
#include "SamBodyRetarget.h"

#include <cmath>
#include <set>

using namespace SamBody;

namespace {

std::vector<float> chainPositions()
{
    std::vector<float> p(127 * 3, 0.0f);
    const auto& par = mhrJointParents();
    for (int i = 1; i < 127; ++i) {
        const int q = par[size_t(i)] >= 0 ? par[size_t(i)] : 0;
        p[size_t(i) * 3] = p[size_t(q) * 3] + 0.05f * float((i % 3) - 1);
        p[size_t(i) * 3 + 1] = p[size_t(q) * 3 + 1] + 0.1f;
        p[size_t(i) * 3 + 2] = p[size_t(q) * 3 + 2] + 0.02f * float(i % 5);
    }
    return p;
}

std::vector<float> randomGlobals(unsigned seed)
{
    std::vector<float> q(127 * 4);
    unsigned s = seed;
    auto rnd = [&]() { s = s * 1664525u + 1013904223u; return (static_cast<float>(s >> 8) / 8388608.0f) - 1.0f; };
    for (int i = 0; i < 127; ++i) {
        float v[4] = {rnd(), rnd(), rnd(), rnd() + 1.5f};
        const float l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2] + v[3] * v[3]);
        for (int k = 0; k < 4; ++k) q[size_t(i) * 4 + size_t(k)] = v[k] / l;
    }
    return q;
}

std::vector<float> identityGlobals()
{
    std::vector<float> q(127 * 4, 0.0f);
    for (int i = 0; i < 127; ++i) q[size_t(i) * 4 + 3] = 1.0f;
    return q;
}

} // namespace

TEST(SamBodyRetarget, ParentsPrecedeChildren)
{
    const auto& par = mhrJointParents();
    EXPECT_EQ(par[0], -1);
    for (int i = 1; i < 127; ++i) EXPECT_LT(par[size_t(i)], i) << i;
    EXPECT_EQ(par[size_t(mhrJointIndex("l_foot"))], mhrJointIndex("l_lowleg"));
    EXPECT_EQ(par[size_t(mhrJointIndex("l_pinky1"))], mhrJointIndex("l_pinky0"));
    EXPECT_EQ(par[size_t(mhrJointIndex("c_head"))], mhrJointIndex("c_neck"));
}

TEST(SamBodyRetarget, LocalPosesReproduceTheGlobalRotationsThroughForwardKinematics)
{
    const Retarget::RigDesc rig = mhrRigDesc(chainPositions(), identityGlobals());
    ASSERT_GT(rig.size(), 60);
    ASSERT_LT(rig.size(), 127);                                        // helpers dropped
    for (const std::string& n : rig.names) EXPECT_FALSE(excludedFromMapping(n)) << n;
    const std::vector<float> globals = randomGlobals(7);
    const Retarget::LocalPose pose = mhrLocalPose(rig, globals);
    std::vector<Ogre::Quaternion> wr;
    std::vector<Ogre::Vector3> wp;
    Retarget::forwardKinematics(rig, pose, wr, wp);
    for (int i = 0; i < rig.size(); ++i) {
        const int f = mhrJointIndex(rig.names[size_t(i)].c_str());
        const Ogre::Quaternion want(globals[size_t(f) * 4 + 3], globals[size_t(f) * 4], globals[size_t(f) * 4 + 1], globals[size_t(f) * 4 + 2]);
        EXPECT_NEAR(std::fabs(wr[size_t(i)].Dot(want)), 1.0f, 1e-4f) << rig.names[size_t(i)];
    }
}

TEST(SamBodyRetarget, TheRestPoseIsTheBindPose)
{
    const std::vector<float> rest = randomGlobals(3);
    const Retarget::RigDesc rig = mhrRigDesc(chainPositions(), rest);
    const Retarget::LocalPose pose = mhrLocalPose(rig, rest);          // posing AT rest must change nothing
    for (int i = 0; i < rig.size(); ++i) EXPECT_NEAR(std::fabs(pose.rot[size_t(i)].Dot(rig.bindRot[size_t(i)])), 1.0f, 1e-4f) << i;
}

TEST(SamBodyRetarget, MalformedInputIsRefused)
{
    EXPECT_EQ(mhrRigDesc({}, {}).size(), 0);
    EXPECT_EQ(mhrRigDesc(chainPositions(), std::vector<float>(10)).size(), 0);
    const Retarget::RigDesc rig = mhrRigDesc(chainPositions(), identityGlobals());
    const Retarget::LocalPose bind = Retarget::bindPose(rig);
    const Retarget::LocalPose bad = mhrLocalPose(rig, std::vector<float>(5));
    for (int i = 0; i < rig.size(); ++i) EXPECT_NEAR(std::fabs(bad.rot[size_t(i)].Dot(bind.rot[size_t(i)])), 1.0f, 1e-6f);
    EXPECT_FALSE(retargetToSkeleton(nullptr, rig, {bind, bind}, 5, "c", true).ok);
}

TEST(SamBodyRetarget, HelperJointsStayOutOfTheMap)
{
    for (const char* n : {"l_upleg_twist2_proc", "l_ring_null", "body_world", "c_jaw", "l_eye", "c_tongue1", "c_teeth"})
        EXPECT_TRUE(excludedFromMapping(n)) << n;
    for (const char* n : {"l_upleg", "r_wrist", "c_head", "l_thumb3", "c_spine2", "r_clavicle"})
        EXPECT_FALSE(excludedFromMapping(n)) << n;
}

TEST(SamBodyRetarget, AliasesUseTheStandardSpellingsAndStayUnique)
{
    EXPECT_EQ(mhrAlias("root"), "Hips");
    EXPECT_EQ(mhrAlias("l_lowarm"), "LeftForeArm");
    EXPECT_EQ(mhrAlias("r_upleg"), "RightUpLeg");
    EXPECT_EQ(mhrAlias("l_pinky2"), "LeftHandPinky2");
    EXPECT_EQ(mhrAlias("r_thumb1"), "RightHandThumb1");
    EXPECT_EQ(mhrAlias("l_ball"), "LeftToeBase");
    EXPECT_EQ(mhrAlias("l_pinky0"), "l_pinky0");               // the metacarpal-like joint has no counterpart
    EXPECT_EQ(mhrAlias("c_spine0"), "c_spine0");
    std::set<std::string> seen;
    for (const char* n : mhrJointNames()) {
        if (excludedFromMapping(n)) continue;
        const std::string a = mhrAlias(n);
        if (a != n) EXPECT_TRUE(seen.insert(a).second) << "duplicate alias " << a;
    }
    EXPECT_GE(seen.size(), 45u);
}

TEST(SamBodyRetarget, UprightCorrectionStandsTheTorsoUpWithoutTurningIt)
{
    // torso axis (0, 0.45, 0.20): leaning towards +Z (the camera) by ~24 degrees
    std::vector<float> pos(127 * 3, 0.0f);
    const int neck = mhrJointIndex("c_neck");
    pos[size_t(neck) * 3 + 1] = 0.45f;
    pos[size_t(neck) * 3 + 2] = 0.20f;
    const Ogre::Quaternion c = uprightCorrection({pos, pos, pos});
    const Ogre::Vector3 up = c * Ogre::Vector3(0.0f, 0.45f, 0.20f).normalisedCopy();
    EXPECT_NEAR(up.x, 0.0f, 1e-5f);
    EXPECT_NEAR(up.y, 1.0f, 1e-5f);
    EXPECT_NEAR(up.z, 0.0f, 1e-5f);
    // the facing direction (a horizontal vector perpendicular to the lean) is not turned
    const Ogre::Vector3 side = c * Ogre::Vector3::UNIT_X;
    EXPECT_NEAR(side.x, 1.0f, 1e-5f);
    // an upright torso, no frames, or malformed frames: nothing to correct
    pos[size_t(neck) * 3 + 2] = 0.0f;
    EXPECT_NEAR(uprightCorrection({pos}).w, 1.0f, 1e-6f);
    EXPECT_NEAR(uprightCorrection({}).w, 1.0f, 1e-6f);
    EXPECT_NEAR(uprightCorrection({std::vector<float>(5)}).w, 1.0f, 1e-6f);
}

TEST(SamBodyRetarget, WorldRotationIsAppliedToEveryJoint)
{
    std::vector<float> q = identityGlobals();
    Ogre::Quaternion c(Ogre::Degree(90), Ogre::Vector3::UNIT_X);
    applyWorldRotation(q, c);
    for (int i = 0; i < 127; ++i) {
        EXPECT_NEAR(q[size_t(i) * 4], c.x, 1e-6f);
        EXPECT_NEAR(q[size_t(i) * 4 + 3], c.w, 1e-6f);
    }
}
