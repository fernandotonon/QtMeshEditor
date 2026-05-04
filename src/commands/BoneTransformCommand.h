#ifndef BONE_TRANSFORM_COMMAND_H
#define BONE_TRANSFORM_COMMAND_H

#include <QUndoCommand>
#include <OgreVector3.h>
#include <OgreQuaternion.h>
#include <string>

namespace Ogre {
    class SkeletonInstance;
}

/**
 * Records a bone TRS edit applied directly in the viewport (not via a
 * keyframe). On undo the bone reverts to its captured before-state; on
 * redo the after-state is reapplied. Stores the skeleton + bone name
 * (not the pointer) so it survives skeleton rebuilds.
 *
 * Used by the bone gizmo: the press handler captures the before-state,
 * the release handler captures the after-state and pushes the command.
 *
 * When `bindMode` is true (T-pose authoring path), undo/redo also call
 * setInitialState() so the bone's initial (bind) pose is restored along
 * with its current local TRS. Without this, the bind-pose edit
 * persists across undo (Skeleton::reset would snap the bone back to
 * the new bind on the next animation update, defeating undo).
 */
class BoneTransformCommand : public QUndoCommand
{
public:
    BoneTransformCommand(Ogre::SkeletonInstance* skeleton,
                         std::string boneName,
                         const Ogre::Vector3& beforePos,
                         const Ogre::Quaternion& beforeOrient,
                         const Ogre::Vector3& beforeScale,
                         const Ogre::Vector3& afterPos,
                         const Ogre::Quaternion& afterOrient,
                         const Ogre::Vector3& afterScale,
                         bool bindMode = false,
                         QUndoCommand* parent = nullptr);

    void undo() override;
    void redo() override;

private:
    void apply(const Ogre::Vector3& p, const Ogre::Quaternion& o,
               const Ogre::Vector3& s);

    Ogre::SkeletonInstance* mSkeleton;
    std::string             mBoneName;
    Ogre::Vector3           mBeforePos;
    Ogre::Quaternion        mBeforeOrient;
    Ogre::Vector3           mBeforeScale;
    Ogre::Vector3           mAfterPos;
    Ogre::Quaternion        mAfterOrient;
    Ogre::Vector3           mAfterScale;
    bool                    mBindMode;
};

#endif // BONE_TRANSFORM_COMMAND_H
