#ifndef DECIMATE_TRACK_COMMAND_H
#define DECIMATE_TRACK_COMMAND_H

#include <QUndoCommand>
#include <OgreVector3.h>
#include <OgreQuaternion.h>
#include <string>
#include <vector>

namespace Ogre { class TransformKeyFrame; }

/**
 * Decimate a bone's animation track to a target FPS by dropping
 * keyframes that fall closer than 1/targetFps to a kept neighbor.
 * Snapshots every keyframe before the first redo so undo restores
 * the dense track exactly. Channel-agnostic — preserves all 10
 * channels of the kept frames.
 */
class DecimateTrackCommand : public QUndoCommand
{
public:
    DecimateTrackCommand(std::string entityName,
                         std::string animationName,
                         std::string boneName,
                         int targetFps,
                         bool isNodeClip = false,
                         QUndoCommand* parent = nullptr);

    void undo() override;
    void redo() override;

    struct KeyframeSnapshot {
        float            time;
        Ogre::Vector3    translate;
        Ogre::Quaternion rotation;
        Ogre::Vector3    scale;
    };

private:
    bool snapshotTrack(std::vector<KeyframeSnapshot>& out);
    bool replaceTrack(const std::vector<KeyframeSnapshot>& snap);
    std::vector<KeyframeSnapshot> decimate(
        const std::vector<KeyframeSnapshot>& dense) const;

    std::string mEntityName;
    std::string mAnimationName;
    std::string mBoneName;
    int         mTargetFps;
    // True → the track lives on a SceneManager node clip, not a
    // skeleton (see ResampleCurveCommand). (#520)
    bool        mIsNodeClip;
    std::vector<KeyframeSnapshot> mBefore;
    std::vector<KeyframeSnapshot> mAfter;
    bool        mCaptured = false;
};

#endif // DECIMATE_TRACK_COMMAND_H
