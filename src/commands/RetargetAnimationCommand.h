#ifndef RETARGET_ANIMATION_COMMAND_H
#define RETARGET_ANIMATION_COMMAND_H

// Undo step for an animation retarget (#523): the retarget CREATES one new
// skeletal clip on the target entity. The command snapshots that clip
// keyframe-for-keyframe right after it was built, so undo() removes it and
// redo() rebuilds it bit-identically without the source skeleton (which may
// have been deleted in between). The entity is resolved by NAME on every
// apply (SkeletonResolver), never via a cached pointer.

#include <QUndoCommand>

#include <OgreQuaternion.h>
#include <OgreVector.h>

#include <string>
#include <vector>

class RetargetAnimationCommand : public QUndoCommand
{
public:
    /// Snapshot `animName` as it exists NOW on `entityName`'s skeleton.
    /// `alreadyApplied`: the clip is already there, so the first redo() is a
    /// no-op (the retarget ran before the push).
    RetargetAnimationCommand(std::string entityName, std::string animName,
                             bool alreadyApplied = true,
                             QUndoCommand* parent = nullptr);

    void undo() override;
    void redo() override;

    bool valid() const { return m_valid; }
    const std::string& animationName() const { return m_animName; }

private:
    struct Key { float time; Ogre::Quaternion rot; Ogre::Vector3 pos; Ogre::Vector3 scale; };
    struct Track { unsigned short handle; std::vector<Key> keys; };

    std::string m_entityName;
    std::string m_animName;
    float m_length = 0.0f;
    std::vector<Track> m_tracks;
    bool m_valid = false;
    bool m_skipFirstRedo = false;
};

#endif // RETARGET_ANIMATION_COMMAND_H
