#include "CreatureMotionRetarget.h"

#include <OgreAnimation.h>
#include <OgreAxisAlignedBox.h>
#include <OgreBone.h>
#include <OgreEntity.h>
#include <OgreMesh.h>
#include <OgreKeyFrame.h>
#include <OgreSkeleton.h>
#include <OgreSkeletonInstance.h>

#include <QDebug>
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

int planSpecificity(BodyPlan p, const QString& bone)
{
    return p == BodyPlan::WingedBiped
               ? CreatureSkeleton::WingedBiped::roleSpecificity(bone)
               : CreatureSkeleton::QuadrupedSkeleton::roleSpecificity(bone);
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

float hipHeightOf(Ogre::Entity* entity, BodyPlan plan)
{
    if (!entity || !entity->getMesh()) return 0.0f;
    Ogre::SkeletonInstance* skel = entity->getSkeleton();
    if (!skel) return 0.0f;

    // The root is whichever bone claims canonical role 0, resolved with the
    // same specificity rule the retarget uses so the two cannot disagree.
    Ogre::Bone* root = nullptr;
    int best = -1;
    for (auto* b : skel->getBones()) {
        const QString bn = QString::fromStdString(b->getName());
        if (planIndexForBone(plan, bn) != 0) continue;
        const int sc = planSpecificity(plan, bn);
        if (sc > best) { best = sc; root = b; }
    }
    if (!root) return 0.0f;

    skel->reset(true);
    skel->_updateTransforms();
    const Ogre::AxisAlignedBox& bb = entity->getMesh()->getBounds();
    if (bb.isNull() || bb.isInfinite()) return 0.0f;
    const float h = root->_getDerivedPosition().y - bb.getMinimum().y;
    if (h > 1e-4f) return h;
    // Root sitting on the floor: fall back to overall mesh height so a jump
    // still scales sensibly rather than collapsing to zero.
    const float mh = bb.getMaximum().y - bb.getMinimum().y;
    return mh > 1e-4f ? mh : 0.0f;
}

Result apply(Ogre::Skeleton* skel,
             const std::string& animName,
             BodyPlan plan,
             const std::vector<std::vector<std::array<float, 4>>>& clipQuats,
             int fps,
             const std::vector<std::array<float, 4>>& srcRestWorld,
             const std::vector<std::array<float, 3>>& rootOffset,
             float targetHipHeight)
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

    // Several bones can claim one role, so take the most EXPLICIT rather
    // than the first listed: these rigs carry both `FrontLeg.L` (a bare name
    // indexForBone can only guess is the lower leg) and `FrontLowLeg.L`, and
    // the bare one is listed first. First-wins therefore drove the shin
    // rotation through the wrong bone and threw the hoof sideways.
    std::vector<Ogre::Bone*> roleBone(static_cast<size_t>(J), nullptr);
    std::vector<int> roleScore(static_cast<size_t>(J), -1);
    for (auto* bone : skel->getBones()) {
        const QString bn = QString::fromStdString(bone->getName());
        const int role = planIndexForBone(plan, bn);
        if (role < 0 || role >= J) continue;
        const int score = planSpecificity(plan, bn);
        if (score <= roleScore[static_cast<size_t>(role)]) continue;
        if (!roleBone[static_cast<size_t>(role)]) ++r.rolesResolved;
        roleBone[static_cast<size_t>(role)] = bone;
        roleScore[static_cast<size_t>(role)] = score;
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

    // Translation is opt-in: it needs BOTH a clip that carries the channel
    // and a target scale to replay it against.
    //
    // It must go on the skeleton's TOP-LEVEL bone, NOT on canonical role 0.
    // On these rigs role 0 resolves to `Hips` (it beats `root` on
    // specificity), which sits mid-hierarchy: the back legs hang off it but
    // the FRONT legs do not. Translating Hips therefore dragged the pelvis
    // and torso away while the forelegs stayed planted -- the body visibly
    // tore in half during a death. The top-level bone carries the whole rig,
    // which is what "the creature moved" means.
    Ogre::Bone* transBone = nullptr;
    if (Ogre::Bone* r0 = roleBone[0]) {
        transBone = r0;
        while (auto* p = dynamic_cast<Ogre::Bone*>(transBone->getParent()))
            transBone = p;
    }
    const bool useRootTranslation =
        transBone && targetHipHeight > 1e-5f
        && rootOffset.size() >= clipQuats.size();
    r.rootTranslation = useRootTranslation;


    const float len = static_cast<float>(clipQuats.size() - 1)
                      / static_cast<float>(fps);
    if (skel->hasAnimation(animName))
        skel->removeAnimation(animName);
    Ogre::Animation* anim = skel->createAnimation(animName, len);
    anim->setInterpolationMode(Ogre::Animation::IM_LINEAR);

    // Per-FRAME pass. Each mapped bone's key is solved against the parent's
    // orientation AT THAT FRAME, not against its bind orientation.
    //
    // Why that matters: these rigs carry more limb segments than the
    // canonical skeleton (Leg -> UpLeg -> LowLeg -> Foot against
    // UpLeg -> LowLeg -> Foot), so one bone per limb is unmapped and stays
    // frozen at bind. A key computed against the BIND parent then never
    // reaches its intended world orientation, because the real parent chain
    // sits somewhere else -- measured as ~22 deg of residual error
    // concentrated entirely in the LOWER LEGS while the limb roots were
    // already correct to ~1 deg. Solving against the live parent absorbs the
    // frozen bone's contribution.
    std::vector<Ogre::Quaternion> keys(static_cast<size_t>(J),
                                       Ogre::Quaternion::IDENTITY);
    std::vector<Ogre::NodeAnimationTrack*> tracks(static_cast<size_t>(J), nullptr);
    for (int j = 0; j < J; ++j)
        if (roleBone[static_cast<size_t>(j)])
            tracks[static_cast<size_t>(j)] = anim->createNodeTrack(
                roleBone[static_cast<size_t>(j)]->getHandle(),
                roleBone[static_cast<size_t>(j)]);

    const bool haveSrcRest = static_cast<int>(srcRestWorld.size()) == J;

    for (size_t f = 0; f < clipQuats.size(); ++f) {
        const float t = static_cast<float>(f) / static_cast<float>(fps);

        // Pose the skeleton at this frame so `_getDerivedOrientation()` gives
        // the LIVE parent chain, then read each mapped bone's required key.
        skel->reset(true);
        for (int j = 0; j < J; ++j)
            if (Ogre::Bone* bn = roleBone[static_cast<size_t>(j)]) {
                // setOrientation sets the LOCAL orientation outright, while a
                // track key POST-multiplies onto the bind local pose. Pose
                // with bindLocal * key so the evaluated chain matches what the
                // player will produce (otherwise the solve converges on a
                // mirrored result -- measured as a clean ~177 deg flip).
                const Ogre::Quaternion bl =
                    parentBindWorld[static_cast<size_t>(j)].Inverse()
                    * bindWorld[static_cast<size_t>(j)];
                bn->setManuallyControlled(true);
                bn->setOrientation(bl * keys[static_cast<size_t>(j)]);
            }
        skel->_updateTransforms();

        for (int j = 0; j < J; ++j) {
            Ogre::Bone* bone = roleBone[static_cast<size_t>(j)];
            if (!bone) continue;

            const Ogre::Quaternion src0 =
                haveSrcRest ? toQ(srcRestWorld[static_cast<size_t>(j)])
                            : toQ(clipQuats.front()[static_cast<size_t>(j)]);
            Ogre::Quaternion dWorld =
                toQ(clipQuats[f][static_cast<size_t>(j)]) * src0.Inverse();
            dWorld.normalise();

            // Intended world orientation for this bone at this frame.
            Ogre::Quaternion wWant =
                dWorld * bindWorld[static_cast<size_t>(j)];
            wWant.normalise();

            // Live parent world (reflects any frozen intermediate bone).
            Ogre::Quaternion pWorld = Ogre::Quaternion::IDENTITY;
            if (auto* p = dynamic_cast<Ogre::Bone*>(bone->getParent()))
                pWorld = p->_getDerivedOrientation();

            // Ogre POST-multiplies the key onto the bind LOCAL pose:
            //   final_local = bindLocal * key,  world = pWorld * final_local
            // so  key = bindLocal^-1 * pWorld^-1 * wWant.
            const Ogre::Quaternion bindLocal =
                parentBindWorld[static_cast<size_t>(j)].Inverse()
                * bindWorld[static_cast<size_t>(j)];
            Ogre::Quaternion key =
                bindLocal.Inverse() * pWorld.Inverse() * wWant;
            key.normalise();
            keys[static_cast<size_t>(j)] = key;
            tracks[static_cast<size_t>(j)]->createNodeKeyFrame(t)
                ->setRotation(key);
        }
    }
    for (int j = 0; j < J; ++j)
        if (tracks[static_cast<size_t>(j)]) {
            tracks[static_cast<size_t>(j)]->_keyFrameDataChanged();
            ++r.tracksWritten;
        }

    // Whole-rig displacement, on the top-level bone. Written after the
    // rotation pass so it can reuse that bone's track when it happens to also
    // carry a canonical role, instead of creating a second track for the same
    // handle (Ogre keys one track per bone).
    if (useRootTranslation) {
        Ogre::NodeAnimationTrack* tt =
            anim->hasNodeTrack(transBone->getHandle())
                ? anim->getNodeTrack(transBone->getHandle())
                : anim->createNodeTrack(transBone->getHandle(), transBone);
        for (size_t f = 0; f < clipQuats.size(); ++f) {
            const float t = static_cast<float>(f) / static_cast<float>(fps);
            const auto& o = rootOffset[f];
            // Scale the normalised offset by the TARGET's hip height. The
            // top-level bone's parent frame is the (unanimated) skeleton
            // root, so a world offset applies directly here -- no transport
            // needed, unlike the per-bone rotations.
            tt->createNodeKeyFrame(t)->setTranslate(
                Ogre::Vector3(o[0], o[1], o[2]) * targetHipHeight);
        }
        tt->_keyFrameDataChanged();
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
