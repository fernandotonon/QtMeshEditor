#include "CreatureMotionExtract.h"

#include <OgreAnimation.h>
#include <OgreBone.h>
#include <OgreEntity.h>
#include <OgreAxisAlignedBox.h>
#include <OgreMesh.h>
#include <OgreMath.h>
#include <OgreSkeletonInstance.h>

#include <QStringList>

#include <algorithm>
#include <cmath>

namespace {

using CreatureSkeleton::BodyPlan;

// Role count + bone->role mapping for a plan, so the two plans share one
// sampling loop instead of duplicating it.
int planJointCount(BodyPlan p)
{
    return p == BodyPlan::WingedBiped
               ? CreatureSkeleton::WingedBiped::jointCount()
               : CreatureSkeleton::QuadrupedSkeleton::jointCount();
}

int planIndexForBone(BodyPlan p, const QString& bone)
{
    return p == BodyPlan::WingedBiped
               ? CreatureSkeleton::WingedBiped::indexForBone(bone)
               : CreatureSkeleton::QuadrupedSkeleton::indexForBone(bone);
}

int planSpecificity(BodyPlan p, const QString& bone)
{
    return p == BodyPlan::WingedBiped
               ? CreatureSkeleton::WingedBiped::roleSpecificity(bone)
               : CreatureSkeleton::QuadrupedSkeleton::roleSpecificity(bone);
}

std::array<float, 4> toArr(const Ogre::Quaternion& q)
{
    return {static_cast<float>(q.x), static_cast<float>(q.y),
            static_cast<float>(q.z), static_cast<float>(q.w)};
}

} // namespace

namespace CreatureMotionExtract {

Result extract(Ogre::Entity* entity, int fps, const QString& onlyAnimation)
{
    Result r;
    if (!entity || !entity->hasSkeleton()) {
        r.error = QStringLiteral("no skeleton on this entity");
        return r;
    }
    if (fps <= 0) {
        r.error = QStringLiteral("fps must be positive");
        return r;
    }
    Ogre::SkeletonInstance* skel = entity->getSkeleton();
    if (!skel) {
        r.error = QStringLiteral("skeleton instance unavailable");
        return r;
    }

    QStringList boneNames;
    for (auto* b : skel->getBones())
        boneNames << QString::fromStdString(b->getName());

    bool planOk = false;
    const BodyPlan plan = CreatureSkeleton::detectBodyPlan(boneNames, &planOk);
    if (!planOk) {
        // Refuse rather than guess. An unnamed rig ("Bone.001", …) or a
        // humanoid retargeted as a creature produces motion that LOOKS
        // plausible frame by frame and is completely wrong — the failure
        // this whole module exists to remove.
        r.error = QStringLiteral(
            "no creature body plan matches this rig (needs semantically "
            "named bones: front/back legs, or wings)");
        return r;
    }
    r.plan = plan;

    const int J = planJointCount(plan);
    // Most EXPLICIT bone wins the role, not the first listed -- see
    // CreatureMotionRetarget for why (`FrontLeg.L` vs `FrontLowLeg.L`).
    // The extractor must agree with the retarget, or a clip is SAMPLED from
    // one set of bones and REPLAYED on another.
    std::vector<Ogre::Bone*> roleBone(static_cast<size_t>(J), nullptr);
    std::vector<int> roleScore(static_cast<size_t>(J), -1);
    int resolved = 0;
    for (auto* bone : skel->getBones()) {
        const QString bn = QString::fromStdString(bone->getName());
        const int role = planIndexForBone(plan, bn);
        if (role < 0 || role >= J) continue;
        const int score = planSpecificity(plan, bn);
        if (score <= roleScore[static_cast<size_t>(role)]) continue;
        if (!roleBone[static_cast<size_t>(role)]) ++resolved;
        roleBone[static_cast<size_t>(role)] = bone;
        roleScore[static_cast<size_t>(role)] = score;
    }

    // Reference orientations from the BIND pose. Unlike the humanoid
    // extractor — which must hunt for a calm animated frame because scraped
    // rigs' bind poses disagree with their animation frames — these packs
    // are authored consistently, so the bind pose is the honest reference.
    skel->reset(true);
    skel->_updateTransforms();
    std::vector<std::array<float, 4>> rest(
        static_cast<size_t>(J), std::array<float, 4>{0.f, 0.f, 0.f, 1.f});
    for (int j = 0; j < J; ++j)
        if (Ogre::Bone* b = roleBone[static_cast<size_t>(j)])
            rest[static_cast<size_t>(j)] = toArr(b->_getDerivedOrientation());

    // Root bind position + hip height, for the scale-normalised translation
    // channel. Hip height is the root's height above the mesh floor, which is
    // the natural per-creature yardstick: "rose by one hip height" transfers
    // across body sizes, raw world units do not.
    Ogre::Vector3 rootBind(0, 0, 0);
    float hipHeight = 1.0f;
    if (Ogre::Bone* rb = roleBone[0]) {
        rootBind = rb->_getDerivedPosition();
        const Ogre::AxisAlignedBox& bb = entity->getMesh()->getBounds();
        if (!bb.isNull() && !bb.isInfinite()) {
            const float h = rootBind.y - bb.getMinimum().y;
            if (h > 1e-4f) hipHeight = h;
        }
        // A rig whose root sits ON the floor (or an unusable bound) would make
        // the normalisation explode; fall back to the mesh's own height.
        if (hipHeight <= 1e-4f && !bb.isNull() && !bb.isInfinite()) {
            const float mh = bb.getMaximum().y - bb.getMinimum().y;
            if (mh > 1e-4f) hipHeight = mh;
        }
    }

    for (unsigned short a = 0; a < skel->getNumAnimations(); ++a) {
        Ogre::Animation* anim = skel->getAnimation(a);
        if (!anim || anim->getLength() <= 0.0f) continue;
        const QString name = QString::fromStdString(anim->getName());
        if (!onlyAnimation.isEmpty()
            && name.compare(onlyAnimation, Qt::CaseInsensitive) != 0)
            continue;

        Clip c;
        c.animation = name;
        c.bodyPlan = plan == BodyPlan::WingedBiped
                         ? QStringLiteral("wingedBiped")
                         : QStringLiteral("quadruped");
        c.fps = fps;
        c.resolvedRoles = resolved;
        c.restWorld = rest;
        c.hipHeight = hipHeight;

        const float len = anim->getLength();
        const int n = std::max(2, static_cast<int>(std::lround(len * fps)));
        c.quats.reserve(static_cast<size_t>(n));
        for (int f = 0; f < n; ++f) {
            const float t = len * static_cast<float>(f)
                            / static_cast<float>(n - 1);
            skel->reset(true);
            anim->apply(skel, t);
            skel->_updateTransforms();
            std::vector<std::array<float, 4>> pose(
                static_cast<size_t>(J), std::array<float, 4>{0.f, 0.f, 0.f, 1.f});
            for (int j = 0; j < J; ++j)
                if (Ogre::Bone* b = roleBone[static_cast<size_t>(j)])
                    pose[static_cast<size_t>(j)] =
                        toArr(b->_getDerivedOrientation());
            c.quats.push_back(std::move(pose));

            // Root displacement since frame 0, in hip-height units.
            Ogre::Vector3 off(0, 0, 0);
            if (Ogre::Bone* rb = roleBone[0])
                off = (rb->_getDerivedPosition() - rootBind) / hipHeight;
            c.rootOffset.push_back({static_cast<float>(off.x),
                                    static_cast<float>(off.y),
                                    static_cast<float>(off.z)});
        }
        // Trim a DEAD TAIL. Several source clips declare a length well past
        // their last real keyframe -- Horse|Walk animates for 50.6% of its
        // 3.33s and is frozen for the rest, which reads as "the walk stops
        // halfway and the legs stick". Measured across the farm pack: most
        // clips are 95-100% live, the stragglers 50-85%.
        //
        // Cut back to the last frame that actually differs from its
        // predecessor (plus one, so the final pose is kept). A clip that is
        // live to the end is untouched. Deliberately NOT applied to a clip
        // that barely moves at all (idle breathing is legitimately subtle and
        // would be trimmed to nothing).
        {
            const float kEps = 0.05f;   // degrees-ish, on the quat dot
            auto differs = [&](size_t f) {
                for (int j = 0; j < J; ++j) {
                    const auto& a = c.quats[f - 1][static_cast<size_t>(j)];
                    const auto& b = c.quats[f][static_cast<size_t>(j)];
                    float d = std::fabs(a[0] * b[0] + a[1] * b[1]
                                        + a[2] * b[2] + a[3] * b[3]);
                    d = std::min(1.0f, d);
                    if (Ogre::Math::RadiansToDegrees(2.0f * std::acos(d)) > kEps)
                        return true;
                }
                return false;
            };
            size_t last = 0;
            for (size_t f = 1; f < c.quats.size(); ++f)
                if (differs(f)) last = f;
            // Only trim when there is a real tail AND the clip has real motion
            // to begin with; keep at least 2 frames.
            const size_t keep = last + 2;
            if (last > 4 && keep < c.quats.size()) {
                c.quats.resize(keep);
                if (c.rootOffset.size() > keep) c.rootOffset.resize(keep);
            }
        }

        c.frames = static_cast<int>(c.quats.size());
        if (c.frames > 0) r.clips.push_back(std::move(c));
    }

    skel->reset(true);
    skel->_updateTransforms();

    if (r.clips.empty()) {
        r.error = onlyAnimation.isEmpty()
                      ? QStringLiteral("rig has no skeletal animations")
                      : QStringLiteral("no animation named '%1'").arg(onlyAnimation);
        return r;
    }
    r.ok = true;
    return r;
}

} // namespace CreatureMotionExtract
