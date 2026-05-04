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

    if (autoKeyOn) {
        // Caller writes the keyframe (via AnimationControlController) and
        // pushes BoneTransformCommand. We just unfreeze the bone so the
        // curve drives playback through the new key.
        bone->setManuallyControlled(false);
        (void)hasActiveAnim;
        (void)entityForUpdate;
        (void)beforePos; (void)beforeOrient; (void)beforeScale;
        return Result::Commit;
    }

    // Auto-key OFF: commit to the bind pose. The dragged local TRS
    // becomes the new initial state for this bone. Skeleton::reset()
    // restores bones to their per-instance initial state on every
    // _updateAnimation call, so the edit persists even after toggling
    // animation states on/off. Animation tracks add their stored
    // keyframe deltas on top of the new bind, so the whole animation
    // just gets offset by the user's edit (predictable behavior).
    bone->setInitialState();
    bone->setManuallyControlled(false);
    return Result::CommitBind;
}
