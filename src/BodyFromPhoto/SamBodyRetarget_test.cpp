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

TEST(SamBodySequence, DownscalesOnlyWhatIsTooBigAndKeepsTheAspectRatio)
{
    EXPECT_TRUE(downscaledForCapture(QImage()).isNull());
    QImage small(640, 480, QImage::Format_RGB32);
    EXPECT_EQ(downscaledForCapture(small).size(), small.size());
    QImage big(2560, 1440, QImage::Format_RGB32);
    const QImage d = downscaledForCapture(big);
    EXPECT_EQ(d.width(), 1280);
    EXPECT_EQ(d.height(), 720);
    EXPECT_EQ(downscaledForCapture(big, 640).width(), 640);
}

TEST(SamBodySequence, RefusesInconsistentInputBeforeAnyInference)
{
    Runner runner;
    EXPECT_FALSE(solveSequence(runner, {QImage(8, 8, QImage::Format_RGB32)}, {0.0}).ok());                       // one frame
    const MhrSequence mismatch = solveSequence(runner, {QImage(8, 8, QImage::Format_RGB32), QImage(8, 8, QImage::Format_RGB32)}, {0.0});
    EXPECT_FALSE(mismatch.ok());
    EXPECT_FALSE(mismatch.error.isEmpty());
    EXPECT_EQ(mismatch.solved, 0);
}

TEST(SamBodySequence, ARetargetNeedsAConsistentSequence)
{
    MhrSequence seq;
    EXPECT_FALSE(retargetSequenceToSkeleton(nullptr, seq, 10, "c", true).ok);                                      // empty
    seq.jointQuat.assign(2, std::vector<float>(127 * 4, 0.0f));
    seq.times = {0.0, 1.0};                                                                                        // jointPos missing
    const RetargetOutcome o = retargetSequenceToSkeleton(nullptr, seq, 10, "c", true);
    EXPECT_FALSE(o.ok);
    EXPECT_FALSE(o.error.isEmpty());

    // consistent outer sizes but a SHORT frame: rejected up front, never read past its end or replaced by a bind pose
    seq.jointPos.assign(2, std::vector<float>(127 * 3, 0.0f));
    seq.jointPos[1].resize(5);
    const RetargetOutcome shortPos = retargetSequenceToSkeleton(nullptr, seq, 10, "c", true);
    EXPECT_FALSE(shortPos.ok);
    EXPECT_NE(shortPos.error.indexOf(QLatin1String("incomplete")), -1);
    seq.jointPos[1].assign(127 * 3, 0.0f);
    seq.jointQuat[0].resize(7);
    const RetargetOutcome shortQuat = retargetSequenceToSkeleton(nullptr, seq, 10, "c", true);
    EXPECT_FALSE(shortQuat.ok);
    EXPECT_NE(shortQuat.error.indexOf(QLatin1String("incomplete")), -1);
}
