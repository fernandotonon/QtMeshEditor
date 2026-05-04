#ifndef MOVE_KEYFRAME_COMMAND_H
#define MOVE_KEYFRAME_COMMAND_H

#include <QUndoCommand>
#include <string>

namespace Ogre {
    class SkeletonInstance;
    class NodeAnimationTrack;
}

/**
 * Moves a single keyframe on a bone-specific animation track from `oldTime`
 * to `newTime`. Recoverable via undo, which restores the keyframe to its
 * original time. The track's keyframe count is unchanged either way — the
 * keyframe's identity is its index after re-sorting.
 *
 * Stores entity / animation / bone names so it survives entity reload
 * and skeleton rebuild — the SkeletonInstance is resolved lazily via
 * SkeletonResolver at apply-time.
 */
class MoveKeyframeCommand : public QUndoCommand
{
public:
    MoveKeyframeCommand(std::string entityName,
                        std::string animationName,
                        std::string boneName,
                        float oldTime,
                        float newTime,
                        QUndoCommand* parent = nullptr);

    void undo() override;
    void redo() override;

private:
    /// Locate the keyframe currently at `searchTime` (± epsilon) on this
    /// command's track and shift it to `targetTime`. Used by both undo and
    /// redo to re-resolve the keyframe after a previous move. Returns true
    /// when the move was applied; false if the source keyframe wasn't
    /// found or the target time would collide with another keyframe.
    bool moveKeyframeTo(float searchTime, float targetTime);

    std::string mEntityName;
    std::string mAnimationName;
    std::string mBoneName;
    float       mOldTime;
    float       mNewTime;
};

#endif // MOVE_KEYFRAME_COMMAND_H
