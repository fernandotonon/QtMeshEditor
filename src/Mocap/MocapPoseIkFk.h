#ifndef MOCAPPOSEIKFK_H
#define MOCAPPOSEIKFK_H

#ifdef ENABLE_MOCAP

#include "../MotionInbetween.h"
#include "PoseIKSolver.h"

#include <OgreQuaternion.h>
#include <OgreVector3.h>

#include <algorithm>
#include <array>
#include <cmath>

namespace MocapPoseIkFk {

using Vec3 = std::array<float, 3>;

inline Vec3 sub(const Vec3& a, const Vec3& b)
{
    return {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
}
inline Vec3 add(const Vec3& a, const Vec3& b)
{
    return {a[0] + b[0], a[1] + b[1], a[2] + b[2]};
}
inline Vec3 mul(const Vec3& a, float s)
{
    return {a[0] * s, a[1] * s, a[2] * s};
}
inline Vec3 mid(const Vec3& a, const Vec3& b)
{
    return mul(add(a, b), 0.5f);
}
inline float len(const Vec3& a)
{
    return std::sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
}
inline Vec3 mulVec3(const Ogre::Quaternion& q, const Vec3& v)
{
    const Ogre::Vector3 r = q * Ogre::Vector3(v[0], v[1], v[2]);
    return {r.x, r.y, r.z};
}
inline Ogre::Quaternion quatFromArray(const std::array<float, 4>& q)
{
    return Ogre::Quaternion(q[3], q[0], q[1], q[2]);
}

inline int effectiveParentRole(int role, uint32_t resolvedMask)
{
    int p = MotionInbetween::canonicalParentOf(role);
    while (p >= 0 && !(resolvedMask & (1u << static_cast<unsigned>(p))))
        p = MotionInbetween::canonicalParentOf(p);
    return p;
}

inline Ogre::Quaternion localArtic(
    const std::array<std::array<float, 4>, PoseIK::kCanonicalRoles>& src,
    int role, uint32_t resolvedMask)
{
    const int ep = effectiveParentRole(role, resolvedMask);
    if (ep >= 0)
        return quatFromArray(src[static_cast<size_t>(ep)]).Inverse()
               * quatFromArray(src[static_cast<size_t>(role)]);
    return quatFromArray(src[static_cast<size_t>(role)]);
}

// PoseIK canonical FK used by the debug overlay and body-drive diagnostics.
inline void fkPoseIkJoints(
    const std::array<std::array<float, 4>, PoseIK::kCanonicalRoles>& quats,
    uint32_t resolvedMask,
    const std::array<std::array<float, 3>, PoseIK::kLandmarkCount>& canonLmPts,
    std::array<Vec3, PoseIK::kCanonicalRoles>& out)
{
    out.fill({0.f, 0.f, 0.f});
    const Vec3 hip = mid(canonLmPts[23], canonLmPts[24]);
    out[static_cast<size_t>(PoseIK::Hip)] = hip;

    struct BoneSeg {
        int role;
        int fromLm;
        int toLm;
    };
    static const BoneSeg segs[] = {
        {PoseIK::Abdomen, 23, 11}, {PoseIK::Chest, 11, 12},
        {PoseIK::Neck, 12, 0}, {PoseIK::Head, 0, 8},
        {PoseIK::RShoulder, 12, 14}, {PoseIK::RElbow, 14, 16},
        {PoseIK::RHand, 16, 16},
        {PoseIK::LShoulder, 11, 13}, {PoseIK::LElbow, 13, 15},
        {PoseIK::LHand, 15, 15},
        {PoseIK::RHip, 24, 26}, {PoseIK::RKnee, 26, 28},
        {PoseIK::RFoot, 28, 32},
        {PoseIK::LHip, 23, 25}, {PoseIK::LKnee, 25, 27},
        {PoseIK::LFoot, 27, 31},
    };

    std::array<Vec3, PoseIK::kCanonicalRoles> restOffset{};
    for (const BoneSeg& s : segs) {
        Vec3 dir = sub(canonLmPts[static_cast<size_t>(s.toLm)],
                       canonLmPts[static_cast<size_t>(s.fromLm)]);
        const float d = len(dir);
        if (d < 1e-5f)
            dir = {0.f, 0.12f, 0.f};
        else
            dir = mul(dir, 1.f / d);
        restOffset[static_cast<size_t>(s.role)] = mul(dir, std::max(d, 0.05f));
    }

    std::array<Ogre::Quaternion, PoseIK::kCanonicalRoles> worldRot{};
    worldRot.fill(Ogre::Quaternion::IDENTITY);

    for (int role = 0; role < PoseIK::kCanonicalRoles; ++role) {
        if (!(resolvedMask & (1u << static_cast<unsigned>(role))))
            continue;
        const int parent = MotionInbetween::canonicalParentOf(role);
        const Ogre::Quaternion local = localArtic(quats, role, resolvedMask);
        if (parent >= 0 && (resolvedMask & (1u << static_cast<unsigned>(parent)))) {
            worldRot[static_cast<size_t>(role)] =
                worldRot[static_cast<size_t>(parent)] * local;
            out[static_cast<size_t>(role)] =
                add(out[static_cast<size_t>(parent)],
                    mulVec3(worldRot[static_cast<size_t>(parent)],
                            restOffset[static_cast<size_t>(role)]));
        } else if (role == PoseIK::Hip) {
            worldRot[0] = quatFromArray(quats[0]);
        } else {
            worldRot[static_cast<size_t>(role)] =
                quatFromArray(quats[static_cast<size_t>(role)]);
            out[static_cast<size_t>(role)] =
                add(hip, restOffset[static_cast<size_t>(role)]);
        }
    }
}

}  // namespace MocapPoseIkFk

#endif  // ENABLE_MOCAP
#endif  // MOCAPPOSEIKFK_H
