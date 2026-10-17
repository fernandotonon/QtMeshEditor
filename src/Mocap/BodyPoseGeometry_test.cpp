#ifdef ENABLE_MOCAP

#include <gtest/gtest.h>

#include <array>
#include <cmath>

#include "Mocap/BodyPoseGeometry.h"

namespace {

using Landmarks = std::array<float, PoseIK::kLandmarkCount * 3>;

void set(Landmarks& l, int lm, float x, float y, float z)
{
    l[lm * 3 + 0] = x;
    l[lm * 3 + 1] = y;
    l[lm * 3 + 2] = z;
}

// Torso + legs of a standing person (MediaPipe world frame: x subject-left,
// y DOWN, z away from camera). Arms are filled in by each test.
Landmarks standing()
{
    Landmarks l{};
    set(l, 0, 0.f, -0.65f, -0.10f);
    set(l, 7, 0.08f, -0.62f, 0.02f);
    set(l, 8, -0.08f, -0.62f, 0.02f);
    set(l, 11, 0.18f, -0.45f, 0.f);
    set(l, 12, -0.18f, -0.45f, 0.f);
    set(l, 23, 0.10f, 0.f, 0.f);
    set(l, 24, -0.10f, 0.f, 0.f);
    set(l, 25, 0.10f, 0.40f, 0.f);
    set(l, 26, -0.10f, 0.40f, 0.f);
    set(l, 27, 0.10f, 0.80f, 0.f);
    set(l, 28, -0.10f, 0.80f, 0.f);
    set(l, 31, 0.10f, 0.85f, -0.15f);
    set(l, 32, -0.10f, 0.85f, -0.15f);
    return l;
}

// Rotation of the elbow (forearm relative to upper arm), as degrees of twist
// about the upper arm's own axis (its local Y) — the candy-wrapper component.
double elbowTwistDegrees(const BodyPoseGeometry::Pose& pose, int upper, int lower)
{
    const Ogre::Quaternion rel = pose.orientations[upper].Inverse() * pose.orientations[lower];
    double deg = 2.0 * std::atan2(rel.y, rel.w) * 180.0 / M_PI;
    while (deg > 180.0) deg -= 360.0;
    while (deg < -180.0) deg += 360.0;
    return deg;
}

}  // namespace

// Arms crossed over the chest: both forearms point at the camera, which is the
// pelvis-forward direction the roll used to be measured against. The forearm
// roll used to snap by ~90 degrees there; it must now be a pure hinge.
TEST(BodyPoseGeometry, ForearmPointingAtCameraHasNoElbowTwist)
{
    Landmarks l = standing();
    // left arm out to the side, forearm swung toward the camera (-z)
    set(l, 13, 0.45f, -0.45f, 0.f);
    set(l, 15, 0.45f, -0.45f, -0.30f);
    // right arm likewise, mirrored
    set(l, 14, -0.45f, -0.45f, 0.f);
    set(l, 16, -0.45f, -0.45f, -0.30f);
    const auto pose = BodyPoseGeometry::solve(l.data());
    ASSERT_TRUE(pose.resolved(PoseIK::LElbow));
    ASSERT_TRUE(pose.resolved(PoseIK::RElbow));
    EXPECT_NEAR(elbowTwistDegrees(pose, PoseIK::LShoulder, PoseIK::LElbow), 0.0, 1.0);
    EXPECT_NEAR(elbowTwistDegrees(pose, PoseIK::RShoulder, PoseIK::RElbow), 0.0, 1.0);
}

TEST(BodyPoseGeometry, ForearmFollowsItsLandmarkDirection)
{
    Landmarks l = standing();
    set(l, 13, 0.45f, -0.45f, 0.f);
    set(l, 15, 0.45f, -0.10f, -0.20f);  // forearm down and toward the camera
    set(l, 14, -0.45f, -0.45f, 0.f);
    set(l, 16, -0.70f, -0.45f, 0.f);
    const auto pose = BodyPoseGeometry::solve(l.data());
    ASSERT_TRUE(pose.resolved(PoseIK::LElbow));
    // landmark y and z are flipped into the solver's up/forward-positive frame
    const Ogre::Vector3 got = pose.orientations[PoseIK::LElbow] * Ogre::Vector3::UNIT_Y;
    const Ogre::Vector3 want(0.f, -0.35f, 0.20f);  // (dx, -dy, -dz) of elbow->wrist
    EXPECT_NEAR(got.normalisedCopy().dotProduct(want.normalisedCopy()), 1.0f, 1e-3f);
}

TEST(BodyPoseGeometry, StraightArmKeepsForearmAlignedWithUpperArm)
{
    Landmarks l = standing();
    set(l, 13, 0.45f, -0.45f, 0.f);
    set(l, 15, 0.70f, -0.45f, 0.f);
    set(l, 14, -0.45f, -0.45f, 0.f);
    set(l, 16, -0.70f, -0.45f, 0.f);
    const auto pose = BodyPoseGeometry::solve(l.data());
    ASSERT_TRUE(pose.resolved(PoseIK::LElbow));
    const Ogre::Vector3 upper = pose.orientations[PoseIK::LShoulder] * Ogre::Vector3::UNIT_Y;
    const Ogre::Vector3 lower = pose.orientations[PoseIK::LElbow] * Ogre::Vector3::UNIT_Y;
    EXPECT_NEAR(upper.dotProduct(lower), 1.0f, 1e-3f);
    EXPECT_NEAR(elbowTwistDegrees(pose, PoseIK::LShoulder, PoseIK::LElbow), 0.0, 1.0);
}

// A fully folded elbow (forearm back along the upper arm) makes the two
// directions antiparallel, where the shortest-arc axis is arbitrary. A hair of
// landmark noise must not flip the forearm roll.
TEST(BodyPoseGeometry, FoldedElbowRollIsStableUnderLandmarkNoise)
{
    auto solveFolded = [](float noiseZ, float noiseY) {
        Landmarks l = standing();
        set(l, 13, 0.45f, -0.45f, 0.f);
        set(l, 15, 0.20f, -0.45f + noiseY, noiseZ);  // folded back toward the shoulder
        set(l, 14, -0.45f, -0.45f, 0.f);
        set(l, 16, -0.70f, -0.45f, 0.f);
        return BodyPoseGeometry::solve(l.data());
    };
    // Noise in two perpendicular directions: the arbitrary shortest-arc axis
    // follows the noise, so the unguarded roll differs by ~180 degrees here.
    const auto a = solveFolded(0.004f, 0.f);
    const auto b = solveFolded(0.f, 0.004f);
    ASSERT_TRUE(a.resolved(PoseIK::LElbow));
    ASSERT_TRUE(b.resolved(PoseIK::LElbow));
    const Ogre::Quaternion delta =
        a.orientations[PoseIK::LElbow].Inverse() * b.orientations[PoseIK::LElbow];
    const double deg = 2.0 * std::acos(std::min(1.0f, std::fabs(delta.w))) * 180.0 / M_PI;
    EXPECT_LT(deg, 8.0) << "forearm roll flipped by " << deg << " deg for ~1 deg of noise";
}

#endif  // ENABLE_MOCAP
