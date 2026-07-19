#include "BoneDragRelease.h"

#include <OgreBone.h>
#include <OgreEntity.h>

#include <cmath>

namespace {
// Drag accumulates float noise via projection / parent-frame conversions;
// 1e-5 keeps us well below user-perceivable motion while filtering bit-
// level drift that would otherwise produce stray Commit/CommitBind on
// press/release with no real motion.
constexpr float kPosEpsilon   = 1e-5f;
constexpr float kScaleEpsilon = 1e-5f;
constexpr float kQuatDotEps   = 1e-6f;

bool nearlyEqual(const Ogre::Vector3& a, const Ogre::Vector3& b, float eps) {
    return (a - b).squaredLength() <= eps * eps;
}
bool nearlyEqual(const Ogre::Quaternion& a, const Ogre::Quaternion& b, float eps) {
    // 1 - |dot| ≈ 0 when quats represent the same rotation (handles
    // sign flip with the absolute value).
    return (1.0f - std::fabs(a.Dot(b))) <= eps;
}
}

BoneDragRelease::Result BoneDragRelease::apply(Ogre::Bone* bone,
                                               const Ogre::Vector3& beforePos,
                                               const Ogre::Quaternion& beforeOrient,
                                               const Ogre::Vector3& beforeScale,
                                               bool hasActiveAnim,
                                               bool autoKeyOn,
                                               Ogre::Entity* entityForUpdate,
                                               bool editRestMode)
{
    if (!bone) return Result::NoOp;

    // Tolerant comparison — drag math can accumulate sub-microsecond
    // float noise that would otherwise mis-fire Commit on a press/
    // release with no intended motion.
    const bool changed = !nearlyEqual(bone->getPosition(),    beforePos,    kPosEpsilon)
                      || !nearlyEqual(bone->getOrientation(), beforeOrient, kQuatDotEps)
                      || !nearlyEqual(bone->getScale(),       beforeScale,  kScaleEpsilon);
    if (!changed) {
        // Even "no change" should release manual control if it was set
        // during the (zero-delta) drag.
        bone->setManuallyControlled(false);
        return Result::NoOp;
    }

    // Edit-rest (or no active animation): rest-pose authoring. Caller
    // commits via SkeletonEditor::commitBoneRestPose / SetRestPoseCommand
    // so animation tracks are re-baked against the prior bind.
    if (editRestMode || !hasActiveAnim) {
        bone->setManuallyControlled(false);
        return Result::CommitBind;
    }

    if (autoKeyOn) {
        // Caller writes the keyframe (via AnimationControlController) and
        // pushes BoneTransformCommand. We just unfreeze the bone so the
        // curve drives playback through the new key.
        bone->setManuallyControlled(false);
        return Result::Commit;
    }

    // Preview only: revert bone to its pre-drag local TRS so playback
    // is unaffected. Don't call setInitialState — the curve stores
    // deltas relative to initial; changing initial would double-apply
    // the curve delta on next sample.
    bone->setPosition(beforePos);
    bone->setOrientation(beforeOrient);
    bone->setScale(beforeScale);
    bone->setManuallyControlled(false);
    bone->needUpdate(true);
    if (entityForUpdate) entityForUpdate->_updateAnimation();
    return Result::Revert;
}
