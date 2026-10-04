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
// World quaternions aim +Y down each outgoing segment. Captured offsets are
// already posed: rotating them again double-applies articulation and yaw.
inline void fkPoseIkJoints(
    const std::array<std::array<float, 4>, PoseIK::kCanonicalRoles>& quats,
    uint32_t resolvedMask,
    const std::array<std::array<float, 3>, PoseIK::kLandmarkCount>& canonLmPts,
    std::array<Vec3, PoseIK::kCanonicalRoles>& out)
{
    out.fill({0.f, 0.f, 0.f});
    const Vec3 hip = mid(canonLmPts[23], canonLmPts[24]);
    out[static_cast<size_t>(PoseIK::Hip)] = hip;

    const Vec3 chest = mid(canonLmPts[11], canonLmPts[12]);
    const Vec3 head = mid(canonLmPts[7], canonLmPts[8]);
    out[PoseIK::Abdomen] = mid(hip, chest);
    out[PoseIK::Chest] = chest;
    out[PoseIK::Neck] = out[PoseIK::Neck1] = mid(chest, head);
    out[PoseIK::Head] = head;
    struct Chain { int upper, lower, end, a, b, c; };
    const Chain chains[] = {
        {PoseIK::LHip, PoseIK::LKnee, PoseIK::LFoot, 23, 25, 27},
        {PoseIK::RHip, PoseIK::RKnee, PoseIK::RFoot, 24, 26, 28},
        {PoseIK::LShoulder, PoseIK::LElbow, PoseIK::LHand, 11, 13, 15},
        {PoseIK::RShoulder, PoseIK::RElbow, PoseIK::RHand, 12, 14, 16},
    };
    for (const Chain& chain : chains) {
        out[chain.upper] = canonLmPts[chain.a];
        out[chain.lower] = canonLmPts[chain.b];
        out[chain.end] = canonLmPts[chain.c];
        auto aim = [&](int role, int child, int from, int to) {
            if (!(resolvedMask & (1u << role))) return;
            const float length = len(sub(canonLmPts[to], canonLmPts[from]));
            if (!std::isfinite(length)) return;
            out[child] = add(out[role], mulVec3(quatFromArray(quats[role]), {0.f, length, 0.f}));
        };
        aim(chain.upper, chain.lower, chain.a, chain.b);
        aim(chain.lower, chain.end, chain.b, chain.c);
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
