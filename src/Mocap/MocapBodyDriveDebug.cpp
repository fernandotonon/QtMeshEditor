#ifdef ENABLE_MOCAP

#include "MocapBodyDriveDebug.h"

#include "../AnimationMerger.h"
#include "../MotionInbetween.h"
#include "PoseIKSolver.h"

#include <OgreBone.h>
#include <OgreEntity.h>
#include <OgreSkeletonInstance.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <vector>

#include <QtGlobal>

namespace {

using Vec3 = std::array<float, 3>;

Vec3 sub(const Vec3& a, const Vec3& b)
{
    return {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
}
float len(const Vec3& a)
{
    return std::sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
}
Vec3 norm(Vec3 a)
{
    const float n = len(a);
    if (n > 1e-6f)
        for (float& c : a) c /= n;
    return a;
}
float degBetween(const Vec3& a, const Vec3& b)
{
    const float d = std::max(-1.f, std::min(1.f,
        a[0] * b[0] + a[1] * b[1] + a[2] * b[2]));
    return std::acos(d) * 180.f / 3.14159265f;
}

Ogre::Quaternion quatFromArray(const std::array<float, 4>& q)
{
    return Ogre::Quaternion(q[3], q[0], q[1], q[2]);
}

int effectiveParentRole(int role, uint32_t resolvedMask)
{
    int p = MotionInbetween::canonicalParentOf(role);
    while (p >= 0 && !(resolvedMask & (1u << static_cast<unsigned>(p))))
        p = MotionInbetween::canonicalParentOf(p);
    return p;
}

Ogre::Quaternion localArtic(
    const std::array<std::array<float, 4>, PoseIK::kCanonicalRoles>& quats,
    int role, uint32_t resolvedMask)
{
    const int ep = effectiveParentRole(role, resolvedMask);
    const Ogre::Quaternion oq = quatFromArray(quats[static_cast<size_t>(role)]);
    if (ep < 0)
        return oq;
    const Ogre::Quaternion op = quatFromArray(quats[static_cast<size_t>(ep)]);
    return op.Inverse() * oq;
}

void fkPoseIkJoints(
    const std::array<std::array<float, 4>, PoseIK::kCanonicalRoles>& quats,
    uint32_t resolvedMask,
    const std::array<std::array<float, 3>, PoseIK::kLandmarkCount>& canonLmPts,
    std::array<Vec3, PoseIK::kCanonicalRoles>& out)
{
    out.fill({0.f, 0.f, 0.f});
    const Vec3 hip = {
        (canonLmPts[23][0] + canonLmPts[24][0]) * 0.5f,
        (canonLmPts[23][1] + canonLmPts[24][1]) * 0.5f,
        (canonLmPts[23][2] + canonLmPts[24][2]) * 0.5f};
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
            dir = norm(dir);
        restOffset[static_cast<size_t>(s.role)] = {
            dir[0] * std::max(d, 0.05f),
            dir[1] * std::max(d, 0.05f),
            dir[2] * std::max(d, 0.05f)};
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
            const Ogre::Vector3 off(
                restOffset[static_cast<size_t>(role)][0],
                restOffset[static_cast<size_t>(role)][1],
                restOffset[static_cast<size_t>(role)][2]);
            const Ogre::Vector3 w =
                worldRot[static_cast<size_t>(parent)] * off;
            out[static_cast<size_t>(role)] = {
                out[static_cast<size_t>(parent)][0] + w.x,
                out[static_cast<size_t>(parent)][1] + w.y,
                out[static_cast<size_t>(parent)][2] + w.z};
        } else if (role == PoseIK::Hip) {
            worldRot[0] = quatFromArray(quats[0]);
        } else {
            out[static_cast<size_t>(role)] = {
                hip[0] + restOffset[static_cast<size_t>(role)][0],
                hip[1] + restOffset[static_cast<size_t>(role)][1],
                hip[2] + restOffset[static_cast<size_t>(role)][2]};
        }
    }
}

const char* roleName(int role)
{
    static const char* names[] = {
        "Hip", "Abdomen", "Chest", "Neck", "Neck1", "Head",
        "RCollar", "RShoulder", "RElbow", "RHand",
        "LCollar", "LShoulder", "LElbow", "LHand",
        "RButtock", "RHip", "RKnee", "RFoot",
        "LButtock", "LHip", "LKnee", "LFoot"};
    if (role >= 0 && role < 22)
        return names[role];
    return "?";
}

Ogre::Vector3 entityLocalPos(Ogre::Entity* entity, const Ogre::Vector3& world)
{
    if (!entity || !entity->getParentSceneNode())
        return world;
    return entity->getParentSceneNode()->convertWorldToLocalPosition(world);
}

}  // namespace

namespace MocapBodyDriveDebug {

void logFrame(
    Ogre::Entity* entity,
    Ogre::SkeletonInstance* skel,
    const BodyLiveFrame& body,
    const BodyRetargeter* retargeter,
    double timeSec,
    bool bodyNeutralReady,
    bool haveNeutralRef,
    int warmupFrames,
    int warmupTarget,
    size_t localsApplied)
{
    if (!qEnvironmentVariableIsSet("QTMESH_MOCAP_DEBUG") || !entity || !skel
        || !body.valid)
        return;

    static int sLastBucket = -1;
    const int bucket = static_cast<int>(timeSec * 2.0);  // ~0.5 Hz
    if (bucket == sLastBucket)
        return;
    sLastBucket = bucket;

    std::array<std::array<float, 3>, PoseIK::kLandmarkCount> canon{};
    PoseIK::Solver::canonicalizeMediaPipeWorld(body.world.data(), canon);

    float mn = 1e9f, mx = -1e9f;
    for (const auto& v : canon) {
        mn = std::min(mn, v[1]);
        mx = std::max(mx, v[1]);
    }
    const float skelH = std::max(1e-4f, mx - mn);
    const Ogre::AxisAlignedBox box = entity->getBoundingBox();
    const float entityH = box.getMaximum().y - box.getMinimum().y;
    const float scale = (entityH > 1e-3f ? entityH : 1.8f) / skelH;

    std::array<Vec3, PoseIK::kCanonicalRoles> fk{};
    fkPoseIkJoints(body.quats, body.resolvedMask, canon, fk);

    skel->_updateTransforms();

    fprintf(stderr,
            "[mocap] t=%.2f retargeter=%s neutralReady=%d haveNeutral=%d "
            "torsoStable=%d/%d locals=%zu resolvedMask=0x%06x\n",
            timeSec,
            (retargeter && retargeter->valid()) ? "ok" : "NO",
            bodyNeutralReady ? 1 : 0,
            haveNeutralRef ? 1 : 0,
            warmupFrames, warmupTarget,
            localsApplied,
            body.resolvedMask & 0xFFFFFFu);

    if (!bodyNeutralReady || !haveNeutralRef) {
        fprintf(stderr,
                "[mocap]   low torso visibility (%d/%d stable frames) — "
                "limbs may follow; spine needs hip+chest in frame.\n",
                warmupFrames, warmupTarget);
    }

    static const int kLimbRoles[] = {
        PoseIK::LShoulder, PoseIK::LElbow, PoseIK::RShoulder, PoseIK::RElbow,
        PoseIK::LHip, PoseIK::LKnee, PoseIK::RHip, PoseIK::RKnee,
        PoseIK::Chest, PoseIK::Head};

    for (int role : kLimbRoles) {
        if (!(body.resolvedMask & (1u << static_cast<unsigned>(role))))
            continue;

        Ogre::Bone* bone = nullptr;
        Ogre::Bone* child = nullptr;
        for (unsigned short i = 0; i < skel->getNumBones(); ++i) {
            Ogre::Bone* b = skel->getBone(i);
            if (MotionInbetween::canonicalIndexForBone(
                    QString::fromStdString(b->getName())) != role)
                continue;
            bone = b;
            for (unsigned short c = 0; c < b->numChildren(); ++c) {
                if (auto* cb = dynamic_cast<Ogre::Bone*>(b->getChild(c))) {
                    child = cb;
                    break;
                }
            }
            break;
        }
        if (!bone)
            continue;

        const Ogre::Vector3 fkP(
            fk[static_cast<size_t>(role)][0] * scale,
            fk[static_cast<size_t>(role)][1] * scale,
            fk[static_cast<size_t>(role)][2] * scale);
        const Ogre::Vector3 skelP =
            entityLocalPos(entity, bone->_getDerivedPosition());

        float segAngle = -1.f;
        if (child) {
            const Ogre::Vector3 bW = bone->_getDerivedPosition();
            const Ogre::Vector3 cW = child->_getDerivedPosition();
            const Vec3 skelDir = norm({
                cW.x - bW.x, cW.y - bW.y, cW.z - bW.z});
            const int childRole = MotionInbetween::canonicalChildOf(role);
            Vec3 fkDir{0.f, 1.f, 0.f};
            if (childRole >= 0) {
                fkDir = norm(sub(fk[static_cast<size_t>(childRole)],
                                 fk[static_cast<size_t>(role)]));
            }
            segAngle = degBetween(skelDir, fkDir);
        }

        const Ogre::Vector3 delta = skelP - fkP;
        fprintf(stderr,
                "[mocap]   %2d %-10s bone=%-16s poseIk=(%+.2f,%+.2f,%+.2f) "
                "skel=(%+.2f,%+.2f,%+.2f) posErr=%.3f segErr=%.1f° manual=%d\n",
                role, roleName(role), bone->getName().c_str(),
                fkP.x, fkP.y, fkP.z,
                skelP.x, skelP.y, skelP.z,
                delta.length(),
                segAngle,
                bone->isManuallyControlled() ? 1 : 0);
    }
    (void)retargeter;
}

}  // namespace MocapBodyDriveDebug

#endif  // ENABLE_MOCAP
