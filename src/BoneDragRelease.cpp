#include "BoneDragRelease.h"

#include <OgreBone.h>
#include <OgreEntity.h>

BoneDragRelease::Result BoneDragRelease::apply(Ogre::Bone* bone,
                                               const Ogre::Vector3& beforePos,
                                               const Ogre::Quaternion& beforeOrient,
                                               const Ogre::Vector3& beforeScale,
                                               bool hasActiveAnim,
                                               bool autoKeyOn,
                                               Ogre::Entity* entityForUpdate)
{
    if (!bone) return Result::NoOp;

    const bool changed = (bone->getPosition()    != beforePos)
                      || (bone->getOrientation() != beforeOrient)
                      || (bone->getScale()       != beforeScale);
    if (!changed) {
        // Even "no change" should release manual control if it was set
        // during the (zero-delta) drag.
        bone->setManuallyControlled(false);
        return Result::NoOp;
    }

    if (autoKeyOn && hasActiveAnim) {
        // Caller writes the keyframe (via AnimationControlController) and
        // pushes BoneTransformCommand. We just unfreeze the bone so the
        // curve drives playback through the new key.
        bone->setManuallyControlled(false);
        return Result::Commit;
    }

    if (hasActiveAnim) {
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

    // No active animation: T-pose / bind-pose authoring. The dragged
    // local TRS becomes the new bind pose.
    bone->setInitialState();
    bone->setManuallyControlled(false);
    return Result::CommitBind;
}
