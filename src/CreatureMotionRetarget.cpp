#include "CreatureMotionRetarget.h"

#include <OgreAnimation.h>
#include <OgreBone.h>
#include <OgreKeyFrame.h>
#include <OgreSkeleton.h>

#include <QStringList>

#include <algorithm>
#include <cmath>

namespace {

using CreatureSkeleton::BodyPlan;

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

Ogre::Quaternion toQ(const std::array<float, 4>& a)
{
    // Stored x,y,z,w — Ogre's constructor takes w first.
    Ogre::Quaternion q(a[3], a[0], a[1], a[2]);
    q.normalise();
    return q;
}

} // namespace

namespace CreatureMotionRetarget {

Result apply(Ogre::Skeleton* skel,
             const std::string& animName,
             BodyPlan plan,
             const std::vector<std::vector<std::array<float, 4>>>& clipQuats,
             int fps)
{
    Result r;
    if (!skel)                { r.error = QStringLiteral("no skeleton"); return r; }
    if (clipQuats.size() < 2) { r.error = QStringLiteral("clip needs >= 2 frames"); return r; }
    if (fps <= 0)             { r.error = QStringLiteral("fps must be positive"); return r; }

    const int J = planJointCount(plan);
    if (static_cast<int>(clipQuats.front().size()) != J) {
        r.error = QStringLiteral("clip has %1 joints, plan expects %2")
                      .arg(clipQuats.front().size()).arg(J);
        return r;
    }

    // ---- resolve the TARGET rig's roles ------------------------------------
    QStringList boneNames;
    for (auto* b : skel->getBones())
        boneNames << QString::fromStdString(b->getName());
    bool planOk = false;
    const BodyPlan targetPlan =
        CreatureSkeleton::detectBodyPlan(boneNames, &planOk);
    if (!planOk) {
        r.error = QStringLiteral(
            "target rig matches no creature body plan (needs semantically "
            "named bones)");
        return r;
    }
    if (targetPlan != plan) {
        // Refuse rather than approximate. A quadruped clip on a winged biped
        // still produces motion — it is just wrong, and wrong in a way that
        // reads as plausible frame by frame.
        r.error = QStringLiteral("clip is %1 but the target rig is %2")
                      .arg(plan == BodyPlan::WingedBiped ? "wingedBiped" : "quadruped",
                           targetPlan == BodyPlan::WingedBiped ? "wingedBiped" : "quadruped");
        return r;
    }

    std::vector<Ogre::Bone*> roleBone(static_cast<size_t>(J), nullptr);
    for (auto* bone : skel->getBones()) {
        const int role =
            planIndexForBone(plan, QString::fromStdString(bone->getName()));
        if (role >= 0 && role < J && !roleBone[static_cast<size_t>(role)]) {
            roleBone[static_cast<size_t>(role)] = bone;
            ++r.rolesResolved;
        }
    }
    if (r.rolesResolved == 0) {
        r.error = QStringLiteral("target rig resolved no canonical roles");
        return r;
    }

    // ---- target bind-pose world orientations -------------------------------
    // Needed to transport a world delta into each bone's PARENT frame. Read
    // once from the reset pose; the clip is applied as a delta onto it, so
    // the target keeps its own proportions and rest stance.
    skel->reset(true);
    skel->_updateTransforms();
    std::vector<Ogre::Quaternion> bindWorld(static_cast<size_t>(J),
                                            Ogre::Quaternion::IDENTITY);
    std::vector<Ogre::Quaternion> parentBindWorld(static_cast<size_t>(J),
                                                  Ogre::Quaternion::IDENTITY);
    for (int j = 0; j < J; ++j) {
        Ogre::Bone* b = roleBone[static_cast<size_t>(j)];
        if (!b) continue;
        bindWorld[static_cast<size_t>(j)] = b->_getDerivedOrientation();
        if (auto* p = dynamic_cast<Ogre::Bone*>(b->getParent()))
            parentBindWorld[static_cast<size_t>(j)] = p->_getDerivedOrientation();
    }

    const float len = static_cast<float>(clipQuats.size() - 1)
                      / static_cast<float>(fps);
    if (skel->hasAnimation(animName))
        skel->removeAnimation(animName);
    Ogre::Animation* anim = skel->createAnimation(animName, len);
    anim->setInterpolationMode(Ogre::Animation::IM_LINEAR);

    for (int j = 0; j < J; ++j) {
        Ogre::Bone* bone = roleBone[static_cast<size_t>(j)];
        if (!bone) continue;

        const Ogre::Quaternion src0 = toQ(clipQuats.front()[static_cast<size_t>(j)]);
        const Ogre::Quaternion pInv =
            parentBindWorld[static_cast<size_t>(j)].Inverse();

        Ogre::NodeAnimationTrack* track =
            anim->createNodeTrack(bone->getHandle(), bone);
        for (size_t f = 0; f < clipQuats.size(); ++f) {
            const Ogre::Quaternion srcF = toQ(clipQuats[f][static_cast<size_t>(j)]);
            // World delta since the clip's first frame...
            Ogre::Quaternion dWorld = srcF * src0.Inverse();
            dWorld.normalise();
            // ...transported into the target bone's parent-world frame, then
            // composed onto its bind pose. Ogre's track rotations are
            // parent-relative deltas ON TOP of the reset pose, so the bind
            // orientation itself must NOT be included here.
            Ogre::Quaternion local = pInv * dWorld * parentBindWorld[static_cast<size_t>(j)];
            local.normalise();
            const float t = static_cast<float>(f) / static_cast<float>(fps);
            track->createNodeKeyFrame(t)->setRotation(local);
        }
        ++r.tracksWritten;
    }

    skel->reset(true);
    skel->_updateTransforms();

    r.frames = static_cast<int>(clipQuats.size());
    r.length = len;
    r.ok = r.tracksWritten > 0;
    if (!r.ok) r.error = QStringLiteral("no tracks written");
    return r;
}

} // namespace CreatureMotionRetarget
