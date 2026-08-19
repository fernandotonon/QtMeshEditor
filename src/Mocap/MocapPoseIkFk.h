#ifndef MOCAPPOSEIKFK_H
#define MOCAPPOSEIKFK_H

#ifdef ENABLE_MOCAP

#include "../MotionInbetween.h"
#include "PoseIKSolver.h"

#include <OgreBone.h>
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
        {PoseIK::RHand, 14, 16},
        {PoseIK::LShoulder, 11, 13}, {PoseIK::LElbow, 13, 15},
        {PoseIK::LHand, 13, 15},
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

// Vertical hip-to-ankle span in canonical +Y-up space (MediaPipe metres).
// Grows when the legs extend (standing), shrinks when crouched/sitting.
// Returns < 0 when hips or ankles are not visible enough.
inline float canonicalHipFootVerticalSpan(const float* world33x3,
                                          const float* visibility33,
                                          float minVisibility = 0.2f)
{
    std::array<std::array<float, 3>, PoseIK::kLandmarkCount> canon{};
    PoseIK::Solver::canonicalizeMediaPipeWorld(world33x3, canon);
    auto visible = [&](int lm) {
        return !visibility33 || visibility33[lm] >= minVisibility;
    };
    float hipY = 0.f;
    int hipCount = 0;
    if (visible(23)) {
        hipY += canon[static_cast<size_t>(23)][1];
        ++hipCount;
    }
    if (visible(24)) {
        hipY += canon[static_cast<size_t>(24)][1];
        ++hipCount;
    }
    if (hipCount == 0)
        return -1.f;
    hipY /= static_cast<float>(hipCount);

    float lowestAnkleY = hipY;
    bool haveAnkle = false;
    for (int lm : {27, 28}) {
        if (!visible(lm))
            continue;
        lowestAnkleY = std::min(lowestAnkleY, canon[static_cast<size_t>(lm)][1]);
        haveAnkle = true;
    }
    if (!haveAnkle)
        return -1.f;
    const float span = hipY - lowestAnkleY;
    return span > 1e-4f ? span : -1.f;
}

// BlazePose screen landmarks: x,y are normalized crop coords [0,1]; finger
// motion shows up in 2D. World fingertip coords (landmarks 17–22) barely move.
inline bool screenCropFingerDelta2D(const float* screen33x3, int wristLm,
                                    int tipLm, float& outDx, float& outDy,
                                    float& outLen2d)
{
    const float* w = screen33x3 + wristLm * 3;
    const float* t = screen33x3 + tipLm * 3;
    outDx = t[0] - w[0];
    outDy = t[1] - w[1];
    outLen2d = std::sqrt(outDx * outDx + outDy * outDy);
    // Reject garbage: a finger cannot span most of the 256 crop.
    return outLen2d >= 1e-5f && outLen2d <= 0.35f;
}

inline Vec3 fingerDirFromScreenCrop(const float* screen33x3, int wristLm,
                                    int tipLm,
                                    const Ogre::Quaternion& wristCanonQuat)
{
    float dx, dy, len2d;
    if (!screenCropFingerDelta2D(screen33x3, wristLm, tipLm, dx, dy, len2d))
        return {0.f, 0.f, 0.f};
    // Crop +x right, +y down → wrist-local (+x spread, +y toward fingertips).
    const Ogre::Vector3 handLocal(dx / len2d, -dy / len2d, 0.f);
    Ogre::Vector3 canon = wristCanonQuat * handLocal;
    const float l = canon.length();
    if (l < 1e-6f)
        return {0.f, 0.f, 0.f};
    canon /= l;
    return {canon.x, canon.y, canon.z};
}

inline Vec3 fingerTipFromScreenCrop(const Vec3& wristWorld,
                                    const float* screen33x3, int wristLm,
                                    int tipLm,
                                    const Ogre::Quaternion& wristCanonQuat,
                                    float fingerLenMetres = 0.085f)
{
    const Vec3 dir = fingerDirFromScreenCrop(screen33x3, wristLm, tipLm,
                                             wristCanonQuat);
    if (dir[0] == 0.f && dir[1] == 0.f && dir[2] == 0.f) {
        const Ogre::Vector3 fallback = wristCanonQuat * Ogre::Vector3(0.f, 1.f, 0.f);
        return add(wristWorld, {fallback.x * fingerLenMetres, fallback.y * fingerLenMetres,
                                fallback.z * fingerLenMetres});
    }
    float dx, dy, len2d;
    screenCropFingerDelta2D(screen33x3, wristLm, tipLm, dx, dy, len2d);
    const float bendScale = std::clamp(len2d / 0.045f, 0.20f, 1.35f);
    return add(wristWorld, mul(dir, fingerLenMetres * bendScale));
}

inline float screenPalmSpreadAngleRad(const float* screen33x3, int indexLm,
                                      int pinkyLm)
{
    const float* i = screen33x3 + indexLm * 3;
    const float* p = screen33x3 + pinkyLm * 3;
    return std::atan2(p[1] - i[1], p[0] - i[0]);
}

inline void applyHandScreenTwist(Ogre::Bone* wristBone, Ogre::Bone* elbowBone,
                                 float twistDeltaRad)
{
    if (!wristBone || !elbowBone || std::abs(twistDeltaRad) < 1e-4f)
        return;
    Ogre::Vector3 axis =
        wristBone->_getDerivedPosition() - elbowBone->_getDerivedPosition();
    if (axis.squaredLength() < 1e-8f)
        return;
    axis.normalise();
    wristBone->setOrientation(Ogre::Quaternion(Ogre::Radian(twistDeltaRad), axis)
                              * wristBone->getOrientation());
}

}  // namespace MocapPoseIkFk

#endif  // ENABLE_MOCAP
#endif  // MOCAPPOSEIKFK_H
