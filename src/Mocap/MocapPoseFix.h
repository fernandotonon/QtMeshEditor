#ifndef MOCAPPOSEFIX_H
#define MOCAPPOSEFIX_H

#ifdef ENABLE_MOCAP

#include <OgreQuaternion.h>
#include <OgreVector3.h>

#include <array>
#include <cstdint>
#include <utility>

#include "PoseIKSolver.h"

namespace MocapPoseFix {

// Front-facing webcams usually deliver a mirrored image; MediaPipe keeps
// anatomical left/right labels but they end up swapped relative to the user.
// FaceCap is unaffected (dense face mesh); body pose needs this correction.
inline void swapMediaPipeLeftRightLandmarks(float* world33x3,
                                            float* visibility33 = nullptr)
{
    static constexpr int kPairs[][2] = {
        {7, 8},                             // ears
        {11, 12}, {13, 14}, {15, 16},       // arms + wrists
        {17, 18}, {19, 20}, {21, 22},       // finger tips
        {23, 24}, {25, 26}, {27, 28},       // legs
        {31, 32},                           // feet
    };
    auto swapLm = [&](int a, int b) {
        for (int k = 0; k < 3; ++k)
            std::swap(world33x3[a * 3 + k], world33x3[b * 3 + k]);
        if (visibility33)
            std::swap(visibility33[a], visibility33[b]);
    };
    for (const auto& p : kPairs)
        swapLm(p[0], p[1]);
}

inline void swapMediaPipeLeftRightScreenCrop(float* screenCrop33x3)
{
    static constexpr int kPairs[][2] = {
        {11, 12}, {13, 14}, {15, 16},
        {17, 18}, {19, 20}, {21, 22},
        {23, 24}, {25, 26}, {27, 28},
        {31, 32},
    };
    auto swapLm = [&](int a, int b) {
        for (int k = 0; k < 3; ++k)
            std::swap(screenCrop33x3[a * 3 + k], screenCrop33x3[b * 3 + k]);
    };
    for (const auto& p : kPairs)
        swapLm(p[0], p[1]);
}

inline void swapCanonicalLeftRight(PoseIK::FrameResult& fr)
{
    static constexpr int kPairs[][2] = {
        {6, 10},  {7, 11},  {8, 12},  {9, 13},
        {14, 18}, {15, 19}, {16, 20}, {17, 21},
    };
    for (const auto& p : kPairs) {
        std::swap(fr.quats[static_cast<size_t>(p[0])],
                  fr.quats[static_cast<size_t>(p[1])]);
        const uint32_t bitA = 1u << static_cast<unsigned>(p[0]);
        const uint32_t bitB = 1u << static_cast<unsigned>(p[1]);
        const bool resA = (fr.resolvedMask & bitA) != 0u;
        const bool resB = (fr.resolvedMask & bitB) != 0u;
        fr.resolvedMask &= ~(bitA | bitB);
        if (resA)
            fr.resolvedMask |= bitB;
        if (resB)
            fr.resolvedMask |= bitA;
    }
}

inline Ogre::Quaternion kPoseToSkeletonYawPi()
{
    return Ogre::Quaternion(Ogre::Degree(180), Ogre::Vector3::UNIT_Y);
}

// Humanoid head bones often need camera-frame pitch inverted; for unit
// quaternions, negating X mirrors rotation about +X (pitch) while leaving
// yaw (Y) unchanged to first order. Yaw is corrected separately below.
inline Ogre::Quaternion invertCameraPitchDelta(const Ogre::Quaternion& delta)
{
    return Ogre::Quaternion(delta.w, -delta.x, delta.y, delta.z);
}

// Selfie/webcam mirror: head yaw is opposite the rig unless corrected.
inline Ogre::Quaternion invertCameraYawDelta(const Ogre::Quaternion& delta)
{
    return Ogre::Quaternion(delta.w, delta.x, -delta.y, delta.z);
}

// Legacy 180°-yaw bridge for CMU library clips whose bind frame differs.
// Live PoseIK mocap does NOT use these — canonical +Z-facing rigs with
// anatomical L/R bone names; a yaw flip made the body drive backward.
inline Ogre::Vector3 poseDirectionToSkeleton(const Ogre::Vector3& v)
{
    return Ogre::Vector3(-v.x, v.y, -v.z);
}

inline Ogre::Quaternion poseRotationToSkeleton(const Ogre::Quaternion& q)
{
    const Ogre::Quaternion ry = kPoseToSkeletonYawPi();
    Ogre::Quaternion out = ry * q * ry.Inverse();
    out.normalise();
    return out;
}

}  // namespace MocapPoseFix

#endif  // ENABLE_MOCAP
#endif  // MOCAPPOSEFIX_H
