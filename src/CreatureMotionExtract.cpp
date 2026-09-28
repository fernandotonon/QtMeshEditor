#include "CreatureMotionExtract.h"

#include <OgreAnimation.h>
#include <OgreBone.h>
#include <OgreEntity.h>
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
