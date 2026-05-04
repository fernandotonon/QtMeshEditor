#ifndef ADD_KEYFRAME_COMMAND_H
#define ADD_KEYFRAME_COMMAND_H

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
 * Records an addKeyframe() operation so it can be undone. Captures all
 * three modes the caller can produce:
 *
 *   1. Track did not exist pre-edit, keyframe was created → on undo,
 *      destroy the track entirely (the bone returns to "no track" state
 *      so non-rigged bones keep their bind pose under animation playback).
 *   2. Track existed, keyframe at this time did not → on undo, remove
 *      the keyframe.
 *   3. Track existed, keyframe at this time existed → on undo, restore
 *      the keyframe's pre-edit TRS.
 *
 * Stores skeleton/animation/bone names (not pointers) so it survives
 * skeleton rebuilds. The mTime field is the keyframe's time in seconds.
 */
class AddKeyframeCommand : public QUndoCommand
{
public:
    enum class Mode {
        TrackCreated,        ///< Lazy-created track + first keyframe
        KeyframeCreated,     ///< Existing track, new keyframe added
        KeyframeUpdated,     ///< Existing track, existing keyframe updated in-place
    };

    AddKeyframeCommand(Ogre::Skeleton* skeleton,
                       std::string animationName,
                       std::string boneName,
                       float time,
                       Mode mode,
                       const Ogre::Vector3& beforeTranslate,
                       const Ogre::Quaternion& beforeRotation,
                       const Ogre::Vector3& beforeScale,
                       const Ogre::Vector3& afterTranslate,
                       const Ogre::Quaternion& afterRotation,
                       const Ogre::Vector3& afterScale,
                       QUndoCommand* parent = nullptr);

    void undo() override;
    void redo() override;

private:
    Ogre::NodeAnimationTrack* findTrack() const;
    Ogre::TransformKeyFrame*  findKeyframe(Ogre::NodeAnimationTrack* track) const;

    Ogre::Skeleton*  mSkeleton;
    std::string      mAnimationName;
    std::string      mBoneName;
    float            mTime;        ///< keyframe time in seconds
    Mode             mMode;
    Ogre::Vector3    mBeforeTranslate;
    Ogre::Quaternion mBeforeRotation;
    Ogre::Vector3    mBeforeScale;
    Ogre::Vector3    mAfterTranslate;
    Ogre::Quaternion mAfterRotation;
    Ogre::Vector3    mAfterScale;
};

#endif // ADD_KEYFRAME_COMMAND_H
