#ifdef ENABLE_MOCAP

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <vector>

#include "Mocap/PoseIKSolver.h"
#include "Mocap/MocapPoseFix.h"

namespace {

using Landmarks = std::array<float, PoseIK::kLandmarkCount * 3>;

void set(Landmarks& l, int lm, float x, float y, float z)
{
    l[lm * 3 + 0] = x;
    l[lm * 3 + 1] = y;
    l[lm * 3 + 2] = z;
}

// A rough T-pose in the MediaPipe world frame (x subject-left, y DOWN,
// z toward camera), metres, hip-centred.
Landmarks tPose()
{
    Landmarks l{};
    set(l, 0, 0.f, -0.65f, -0.10f);     // nose
    set(l, 7, 0.08f, -0.62f, 0.02f);    // left ear
    set(l, 8, -0.08f, -0.62f, 0.02f);   // right ear
    set(l, 11, 0.18f, -0.45f, 0.f);     // left shoulder
    set(l, 12, -0.18f, -0.45f, 0.f);    // right shoulder
    set(l, 13, 0.45f, -0.45f, 0.f);     // left elbow (arm out)
    set(l, 14, -0.45f, -0.45f, 0.f);    // right elbow
    set(l, 15, 0.70f, -0.45f, 0.f);     // left wrist
    set(l, 16, -0.70f, -0.45f, 0.f);    // right wrist
    set(l, 23, 0.10f, 0.f, 0.f);        // left hip
    set(l, 24, -0.10f, 0.f, 0.f);       // right hip
    set(l, 25, 0.10f, 0.40f, 0.f);      // left knee
    set(l, 26, -0.10f, 0.40f, 0.f);     // right knee
    set(l, 27, 0.10f, 0.80f, 0.f);      // left ankle
    set(l, 28, -0.10f, 0.80f, 0.f);     // right ankle
    set(l, 31, 0.10f, 0.85f, -0.15f);   // left foot index (toes forward)
    set(l, 32, -0.10f, 0.85f, -0.15f);  // right foot index
    return l;
}

double quatAngle(const std::array<float, 4>& a, const std::array<float, 4>& b)
{
    double dot = 0;
    for (int i = 0; i < 4; ++i)
        dot += static_cast<double>(a[i]) * b[i];
    return 2.0 * std::acos(std::fmin(1.0, std::fabs(dot)));
}

}  // namespace

TEST(PoseIKSolver, StaticPoseGivesIdenticalFrames)
{
    PoseIK::Solver solver;
    const Landmarks pose = tPose();
    const auto f1 = solver.solveFrame(pose.data());
    const auto f2 = solver.solveFrame(pose.data());
    for (int r = 0; r < PoseIK::kCanonicalRoles; ++r) {
        if (!f1.resolved(r))
            continue;
        EXPECT_TRUE(f2.resolved(r)) << "role " << r;
        EXPECT_LT(quatAngle(f1.quats[r], f2.quats[r]), 1e-4)
            << "role " << r << " drifted between identical frames";
    }
    // core roles resolve on a full-visibility T-pose
    for (int r : {PoseIK::Hip, PoseIK::Chest, PoseIK::Head, PoseIK::RShoulder,
                  PoseIK::RElbow, PoseIK::LShoulder, PoseIK::LElbow,
                  PoseIK::RHip, PoseIK::RKnee, PoseIK::LHip, PoseIK::LKnee})
        EXPECT_TRUE(f1.resolved(r)) << "role " << r;
}

TEST(PoseIKSolver, ElbowBendRecoversNinetyDegrees)
{
    PoseIK::Solver solver;
    Landmarks pose = tPose();
    const auto f1 = solver.solveFrame(pose.data());

    // bend the left forearm 90 degrees at the elbow: wrist moves from
    // straight-out (+x) to straight-forward (toward the camera, +z in
    // MediaPipe's frame is BEHIND, so forward = -z... use -z)
    set(pose, 15, 0.45f, -0.45f, -0.25f);
    const auto f2 = solver.solveFrame(pose.data());

    ASSERT_TRUE(f1.resolved(PoseIK::LElbow));
    ASSERT_TRUE(f2.resolved(PoseIK::LElbow));
    const double bend = quatAngle(f1.quats[PoseIK::LElbow],
                                  f2.quats[PoseIK::LElbow]);
    EXPECT_NEAR(bend, M_PI / 2.0, 0.05);
    // the upper arm did not move
    EXPECT_LT(quatAngle(f1.quats[PoseIK::LShoulder],
                        f2.quats[PoseIK::LShoulder]), 1e-3);
}

TEST(PoseIKSolver, LeftArmRaiseDoesNotMoveRightArm)
{
    PoseIK::Solver solver;
    Landmarks pose = tPose();
    const auto f1 = solver.solveFrame(pose.data());
    // raise the subject's LEFT arm (MP +x side): lift left wrist in MP space
    set(pose, 15, 0.18f, -0.75f, 0.f);
    set(pose, 13, 0.18f, -0.55f, 0.f);
    const auto f2 = solver.solveFrame(pose.data());

    const double leftDelta = quatAngle(f1.quats[PoseIK::LShoulder],
                                       f2.quats[PoseIK::LShoulder])
                           + quatAngle(f1.quats[PoseIK::LElbow],
                                       f2.quats[PoseIK::LElbow]);
    const double rightDelta = quatAngle(f1.quats[PoseIK::RShoulder],
                                        f2.quats[PoseIK::RShoulder])
                            + quatAngle(f1.quats[PoseIK::RElbow],
                                        f2.quats[PoseIK::RElbow]);
    EXPECT_GT(leftDelta, 0.15);
    EXPECT_LT(rightDelta, 0.05);
}

TEST(PoseIKSolver, TorsoTwistShowsOnChestNotHips)
{
    PoseIK::Solver solver;
    Landmarks pose = tPose();
    const auto f1 = solver.solveFrame(pose.data());

    // rotate the shoulder line ~30 degrees about the vertical axis
    const float c = std::cos(0.5236f), s = std::sin(0.5236f);
    auto yaw = [&](int lm) {
        const float x = pose[lm * 3 + 0];
        const float z = pose[lm * 3 + 2];
        pose[lm * 3 + 0] = c * x - s * z;
        pose[lm * 3 + 2] = s * x + c * z;
    };
    yaw(11);
    yaw(12);
    const auto f2 = solver.solveFrame(pose.data());

    const double chestDelta = quatAngle(f1.quats[PoseIK::Chest],
                                        f2.quats[PoseIK::Chest]);
    const double hipDelta = quatAngle(f1.quats[PoseIK::Hip],
                                      f2.quats[PoseIK::Hip]);
    EXPECT_NEAR(chestDelta, 0.5236, 0.06);
    EXPECT_LT(hipDelta, 0.01);  // hips stayed put
}

TEST(PoseIKSolver, ContinuityWhileArmSweeps)
{
    // sweep the left arm from T-pose down to the side in small steps; the
    // parallel-transported twist reference must keep consecutive frames close
    PoseIK::Solver solver;
    Landmarks pose = tPose();
    auto prev = solver.solveFrame(pose.data());
    for (int step = 1; step <= 20; ++step) {
        const float a = step * (static_cast<float>(M_PI) / 2.f) / 20.f;
        // elbow + wrist rotate about the shoulder in the frontal plane
        set(pose, 13, 0.18f + 0.27f * std::cos(a), -0.45f + 0.27f * std::sin(a), 0.f);
        set(pose, 15, 0.18f + 0.52f * std::cos(a), -0.45f + 0.52f * std::sin(a), 0.f);
        const auto cur = solver.solveFrame(pose.data());
        EXPECT_LT(quatAngle(prev.quats[PoseIK::LShoulder],
                            cur.quats[PoseIK::LShoulder]), 0.25)
            << "jump at step " << step;
        prev = cur;
    }
    // total travel is ~90 degrees
    PoseIK::Solver fresh;
    Landmarks tp = tPose();
    const auto f0 = fresh.solveFrame(tp.data());
    EXPECT_NEAR(quatAngle(f0.quats[PoseIK::LShoulder],
                          prev.quats[PoseIK::LShoulder]), M_PI / 2.0, 0.1);
}

TEST(PoseIKSolver, DegenerateInputDoesNotNaN)
{
    PoseIK::Solver solver;
    Landmarks zeros{};
    const auto f = solver.solveFrame(zeros.data());
    EXPECT_EQ(f.resolvedMask, 0u);  // nothing resolvable
    for (const auto& q : f.quats)
        for (float c : q)
            EXPECT_TRUE(std::isfinite(c));
}

TEST(PoseIKSolver, LowVisibilityInvalidatesRoles)
{
    PoseIK::Solver solver;
    const Landmarks pose = tPose();
    std::array<float, PoseIK::kLandmarkCount> vis;
    vis.fill(1.f);
    vis[15] = 0.05f;  // left wrist invisible
    const auto f = solver.solveFrame(pose.data(), vis.data());
    EXPECT_FALSE(f.resolved(PoseIK::LElbow));   // needs the wrist
    EXPECT_TRUE(f.resolved(PoseIK::LShoulder)); // shoulder->elbow unaffected
}

TEST(PoseIKSolver, HeadNodChangesHeadRotation)
{
    PoseIK::Solver solver;
    Landmarks pose = tPose();
    const auto f1 = solver.solveFrame(pose.data());
    ASSERT_TRUE(f1.resolved(PoseIK::Head));

    // nod down: move nose toward chest (+y in MediaPipe = down)
    set(pose, 0, 0.f, -0.55f, -0.08f);
    const auto f2 = solver.solveFrame(pose.data());
    ASSERT_TRUE(f2.resolved(PoseIK::Head));

    const double nod = quatAngle(f1.quats[PoseIK::Head], f2.quats[PoseIK::Head]);
    EXPECT_GT(nod, 0.05);
    // yaw should stay roughly stable when only nodding
    EXPECT_LT(quatAngle(f1.quats[PoseIK::Hip], f2.quats[PoseIK::Hip]), 0.05);
}

TEST(PoseIKSolver, LimbSegmentDirectionMatchesRaise)
{
    Landmarks pose = tPose();
    std::array<std::array<float, 3>, PoseIK::kLandmarkCount> canon{};
    PoseIK::Solver::canonicalizeMediaPipeWorld(pose.data(), canon);
    std::array<float, 3> refDir{}, upDir{};
    ASSERT_TRUE(PoseIK::Solver::limbSegmentDirection(
        PoseIK::LShoulder, canon, nullptr, 0.3f, refDir));
    // raise left arm: elbow + wrist move up in canonical (+Y)
    set(pose, 15, 0.18f, -0.75f, 0.f);
    set(pose, 13, 0.18f, -0.55f, 0.f);
    PoseIK::Solver::canonicalizeMediaPipeWorld(pose.data(), canon);
    ASSERT_TRUE(PoseIK::Solver::limbSegmentDirection(
        PoseIK::LShoulder, canon, nullptr, 0.3f, upDir));
    EXPECT_GT(upDir[1], refDir[1] + 0.2f);
    std::array<float, 3> rDir{};
    ASSERT_TRUE(PoseIK::Solver::limbSegmentDirection(
        PoseIK::RShoulder, canon, nullptr, 0.3f, rDir));
    EXPECT_LT(rDir[1], refDir[1] + 0.05f);  // right arm stayed level
}

TEST(PoseIKSolver, OccludedFootIndexFallsBackToShin)
{
    Landmarks pose = tPose();
    std::array<std::array<float, 3>, PoseIK::kLandmarkCount> canon{};
    PoseIK::Solver::canonicalizeMediaPipeWorld(pose.data(), canon);
    std::array<float, PoseIK::kLandmarkCount> vis;
    vis.fill(1.f);
    vis[32] = 0.05f;  // right foot index occluded
    std::array<float, 3> dir{};
    ASSERT_TRUE(PoseIK::Solver::limbSegmentDirection(
        PoseIK::RFoot, canon, vis.data(), 0.3f, dir));
    // shin = knee(26) → ankle(28); after canonicalize, +Y is up so shin is -Y
    EXPECT_LT(dir[1], -0.7f);
    vis[31] = 0.05f;
    ASSERT_TRUE(PoseIK::Solver::limbSegmentDirection(
        PoseIK::LFoot, canon, vis.data(), 0.3f, dir));
    EXPECT_LT(dir[1], -0.7f);
}

TEST(PoseIKSolver, SwapScreenCropMirrorsLeftRight)
{
    std::array<float, PoseIK::kLandmarkCount * 3> crop{};
    crop[15 * 3 + 0] = 1.f;
    crop[16 * 3 + 0] = 2.f;
    MocapPoseFix::swapMediaPipeLeftRightScreenCrop(crop.data());
    EXPECT_FLOAT_EQ(crop[15 * 3 + 0], 2.f);
    EXPECT_FLOAT_EQ(crop[16 * 3 + 0], 1.f);
}

TEST(PoseIKSolver, WebcamMirrorSwapFixesRightArmRaise)
{
    PoseIK::Solver solver;
    Landmarks pose = tPose();
    const auto f0 = solver.solveFrame(pose.data());
    // Mirrored webcam: user raises their physical RIGHT arm but MediaPipe tracks
    // the LEFT landmark chain (positions on the MP +x / subject-left side).
    set(pose, 15, 0.18f, -0.75f, 0.f);
    set(pose, 13, 0.18f, -0.55f, 0.f);
    const auto fWrong = solver.solveFrame(pose.data());
    const double wrongLeft = quatAngle(f0.quats[PoseIK::LShoulder],
                                       fWrong.quats[PoseIK::LShoulder]);
    const double wrongRight = quatAngle(f0.quats[PoseIK::RShoulder],
                                        fWrong.quats[PoseIK::RShoulder]);
    EXPECT_GT(wrongLeft, 0.08);
    EXPECT_GT(wrongLeft, wrongRight);

    Landmarks fixed = tPose();
    set(fixed, 15, 0.18f, -0.75f, 0.f);
    set(fixed, 13, 0.18f, -0.55f, 0.f);
    MocapPoseFix::swapMediaPipeLeftRightLandmarks(fixed.data());
    const auto fOk = solver.solveFrame(fixed.data());
    const double fixedRight = quatAngle(f0.quats[PoseIK::RShoulder],
                                        fOk.quats[PoseIK::RShoulder]);
    EXPECT_GT(fixedRight, 0.08);
    EXPECT_GT(fixedRight, wrongRight);
}

#endif  // ENABLE_MOCAP
