#ifndef BODYPOSEGEOMETRY_H
#define BODYPOSEGEOMETRY_H

#include "PoseIKSolver.h"
#include <OgreMatrix3.h>
#include <OgreQuaternion.h>
#include <OgreVector3.h>
#include <algorithm>
#include <array>
#include <cmath>

// Anatomical frames built from the SAME landmarks as the debug overlay.
// Keeping both swing and the bend plane avoids arbitrary thigh/shin twist.
namespace BodyPoseGeometry {

inline bool finite(const Ogre::Vector3& v)
{
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

inline bool frame(Ogre::Vector3 primary, Ogre::Vector3 lateral,
                  Ogre::Quaternion& out)
{
    if (!finite(primary) || !finite(lateral) || primary.squaredLength() < 1e-10f)
        return false;
    primary.normalise();
    lateral -= primary * lateral.dotProduct(primary);
    if (lateral.squaredLength() < 1e-8f)
        return false;
    lateral.normalise();
    Ogre::Matrix3 basis;
    basis.SetColumn(0, lateral);
    basis.SetColumn(1, primary);
    basis.SetColumn(2, lateral.crossProduct(primary));
    out = Ogre::Quaternion(basis);
    out.normalise();
    return true;
}

struct Pose {
    std::array<Ogre::Quaternion, PoseIK::kCanonicalRoles> orientations;
    uint32_t mask = 0;
    Pose() { orientations.fill(Ogre::Quaternion::IDENTITY); }
    bool resolved(int role) const { return (mask & (1u << role)) != 0; }
};

// Estimate only camera heading, not the actor's initial lean. Summing the
// labeled shoulder and hip lines naturally gives the wider, more reliable
// shoulders more weight than noisy hip depth. Removing the full pelvis frame
// erased captured tilt and could yaw a front-facing chest/head sideways.
inline bool cameraHeading(const float* world, Ogre::Quaternion& out)
{
    if (!world) return false;
    auto point = [&](int lm) {
        return Ogre::Vector3(world[lm * 3], -world[lm * 3 + 1], -world[lm * 3 + 2]);
    };
    const Ogre::Vector3 left = point(11) - point(12) + point(23) - point(24);
    return frame(Ogre::Vector3::UNIT_Y, left, out);
}

inline Pose solve(const float* world, const float* visibility = nullptr,
                  const Pose* previous = nullptr)
{
    Pose out;
    if (!world)
        return out;
    std::array<Ogre::Vector3, PoseIK::kLandmarkCount> p;
    for (int i = 0; i < PoseIK::kLandmarkCount; ++i)
        p[i] = {world[i * 3], -world[i * 3 + 1], -world[i * 3 + 2]};
    auto visible = [&](int i) {
        return finite(p[i]) && (!visibility || visibility[i] >= 0.3f);
    };
    auto set = [&](int role, const Ogre::Vector3& dir, const Ogre::Vector3& axis) {
        if (frame(dir, axis, out.orientations[role]))
            out.mask |= 1u << role;
    };
    const bool fullTorso = visible(11) && visible(12) && visible(23) && visible(24);
    if (fullTorso) {
        const Ogre::Vector3 up = (p[11] + p[12] - p[23] - p[24]) * 0.5f;
        // Anatomical left is +X. Unlike an unsigned line, this preserves turns
        // through profile and back-facing poses without an artificial 180° flip.
        set(PoseIK::Hip, up, p[23] - p[24]);
        set(PoseIK::Chest, up, p[11] - p[12]);
    }
    // Torso orientation needs a complete shoulder/hip frame. If a torso point
    // drops out, keep that frame from the previous sample and still solve each
    // limb independently from its visible landmarks.
    if (!out.resolved(PoseIK::Hip)) {
        if (!previous || !previous->resolved(PoseIK::Hip))
            return out;
        out.orientations[PoseIK::Hip] = previous->orientations[PoseIK::Hip];
        out.mask |= 1u << PoseIK::Hip;
    }
    if (!out.resolved(PoseIK::Chest)) {
        if (previous && previous->resolved(PoseIK::Chest))
            out.orientations[PoseIK::Chest] = previous->orientations[PoseIK::Chest];
        else
            out.orientations[PoseIK::Chest] = out.orientations[PoseIK::Hip];
        out.mask |= 1u << PoseIK::Chest;
    }
    out.orientations[PoseIK::Abdomen] = Ogre::Quaternion::Slerp(
        0.5f, out.orientations[PoseIK::Hip], out.orientations[PoseIK::Chest], true);
    out.mask |= 1u << PoseIK::Abdomen;
    for (int role : {PoseIK::Neck, PoseIK::Neck1}) {
        out.orientations[role] = out.orientations[PoseIK::Chest];
        out.mask |= 1u << role;
    }
    const Ogre::Vector3 left = out.orientations[PoseIK::Hip] * Ogre::Vector3::UNIT_X;
    const Ogre::Vector3 forward = out.orientations[PoseIK::Hip] * Ogre::Vector3::UNIT_Z;
    struct Chain { int upperRole, lowerRole, a, b, c; };
    const Chain chains[] = {
        {PoseIK::LHip, PoseIK::LKnee, 23, 25, 27},
        {PoseIK::RHip, PoseIK::RKnee, 24, 26, 28},
        {PoseIK::LShoulder, PoseIK::LElbow, 11, 13, 15},
        {PoseIK::RShoulder, PoseIK::RElbow, 12, 14, 16},
    };
    for (const Chain& ch : chains) {
        // A lost ankle must not erase an otherwise visible thigh.
        const bool upperOk = visible(ch.a) && visible(ch.b);
        const bool lowerOk = visible(ch.b) && visible(ch.c);
        const Ogre::Vector3 upper = p[ch.b] - p[ch.a];
        const Ogre::Vector3 lower = p[ch.c] - p[ch.b];
        const bool leg = ch.upperRole == PoseIK::LHip || ch.upperRole == PoseIK::RHip;
        Ogre::Vector3 lateral = leg ? left : forward;
        float planeWeight = 0.f;
        if (leg && upperOk && lowerOk && upper.squaredLength() > 1e-10f
            && lower.squaredLength() > 1e-10f) {
            Ogre::Vector3 normal = upper.normalisedCopy().crossProduct(lower.normalisedCopy());
            const float bend = normal.length();
            if (bend > 0.05f) {
                normal /= bend;
                if (normal.dotProduct(left) < 0.f)
                    normal = -normal;
                // Straight knees have no measurable bend plane. Fade to the
                // pelvis lateral axis instead of amplifying landmark noise.
                const float weight = std::clamp((bend - 0.05f) / 0.20f, 0.f, 1.f);
                planeWeight = weight;
                lateral = left * (1.f - weight) + normal * weight;
            }
        }
        auto aim = [&](int role, const Ogre::Vector3& dir) {
            Ogre::Vector3 reference = lateral;
            if (previous && previous->resolved(role) && dir.squaredLength() > 1e-10f) {
                const Ogre::Quaternion torsoDelta = out.orientations[PoseIK::Hip]
                    * previous->orientations[PoseIK::Hip].Inverse();
                const Ogre::Quaternion before = torsoDelta * previous->orientations[role];
                const Ogre::Vector3 oldDirection = before * Ogre::Vector3::UNIT_Y;
                const Ogre::Vector3 transported = oldDirection.getRotationTo(dir.normalisedCopy())
                    * (before * Ogre::Vector3::UNIT_X);
                reference = transported * (1.f - planeWeight) + lateral * planeWeight;
            }
            if (!frame(dir, reference, out.orientations[role]))
                set(role, dir, leg ? forward : left);
            else
                out.mask |= 1u << role;
        };
        if (upperOk) aim(ch.upperRole, upper);
        if (lowerOk) aim(ch.lowerRole, lower);
    }
    for (const auto& foot : {std::array<int, 3>{PoseIK::LFoot, 27, 31},
                            std::array<int, 3>{PoseIK::RFoot, 28, 32}})
        if (visible(foot[1]) && visible(foot[2]))
            set(foot[0], p[foot[2]] - p[foot[1]], left);
    for (const auto& hand : {std::array<int, 4>{PoseIK::LHand, 15, 19, 17},
                            std::array<int, 4>{PoseIK::RHand, 16, 20, 18}})
        if (visible(hand[1]) && visible(hand[2]) && visible(hand[3]))
            set(hand[0], (p[hand[2]] + p[hand[3]]) * 0.5f - p[hand[1]],
                p[hand[2]] - p[hand[3]]);
    if (visible(0) && visible(7) && visible(8)) {
        const Ogre::Vector3 faceForward = p[0] - (p[7] + p[8]) * 0.5f;
        const Ogre::Vector3 faceLeft = p[7] - p[8];
        set(PoseIK::Head, faceForward.crossProduct(faceLeft), faceLeft);
    }
    return out;
}
} // namespace BodyPoseGeometry

#endif
