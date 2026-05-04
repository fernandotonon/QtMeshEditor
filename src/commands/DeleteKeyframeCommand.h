#ifndef DELETE_KEYFRAME_COMMAND_H
#define DELETE_KEYFRAME_COMMAND_H

#include <QUndoCommand>
#include <OgreVector3.h>
#include <OgreQuaternion.h>
#include <string>

namespace Ogre {
    class Skeleton;
    class NodeAnimationTrack;
    class TransformKeyFrame;
}

/**
 * Records a deleteKeyframe() operation so it can be undone. On redo
 * the keyframe at the captured time is removed; on undo the keyframe
 * is recreated with its captured TRS.
 *
 * Stores skeleton/animation/bone names (not pointers) so it survives
 * skeleton rebuilds. The mTime field is the keyframe's time in seconds.
 */
class DeleteKeyframeCommand : public QUndoCommand
{
public:
    DeleteKeyframeCommand(Ogre::Skeleton* skeleton,
                          std::string animationName,
                          std::string boneName,
                          float time,
                          const Ogre::Vector3& translate,
                          const Ogre::Quaternion& rotation,
                          const Ogre::Vector3& scale,
                          QUndoCommand* parent = nullptr);

    void undo() override;
    void redo() override;

private:
    Ogre::NodeAnimationTrack* findTrack() const;

    Ogre::Skeleton*  mSkeleton;
    std::string      mAnimationName;
    std::string      mBoneName;
    float            mTime;        ///< keyframe time in seconds
    Ogre::Vector3    mTranslate;
    Ogre::Quaternion mRotation;
    Ogre::Vector3    mScale;
};

#endif // DELETE_KEYFRAME_COMMAND_H
