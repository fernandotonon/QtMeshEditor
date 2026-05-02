#ifndef MOVE_KEYFRAME_COMMAND_H
#define MOVE_KEYFRAME_COMMAND_H

#include <QUndoCommand>
#include <string>

namespace Ogre {
    class Skeleton;
    class NodeAnimationTrack;
}

/**
 * Moves a single keyframe on a bone-specific animation track from `oldTime`
 * to `newTime`. Recoverable via undo, which restores the keyframe to its
 * original time. The track's keyframe count is unchanged either way — the
 * keyframe's identity is its index after re-sorting.
 *
 * The command stores the skeleton + animation + bone names rather than raw
 * pointers so it survives skeleton/track rebuilds (some Ogre operations
 * reallocate tracks when keyframes are inserted/removed elsewhere).
 */
class MoveKeyframeCommand : public QUndoCommand
{
public:
    MoveKeyframeCommand(Ogre::Skeleton* skeleton,
                        std::string animationName,
                        std::string boneName,
                        float oldTime,
                        float newTime,
                        QUndoCommand* parent = nullptr);

    void undo() override;
    void redo() override;

private:
    /// Locate the keyframe currently at `searchTime` (± epsilon) on this
    /// command's track. Returns nullptr if no match. Helper used by both
    /// undo and redo to re-resolve the keyframe after a previous move.
    bool moveKeyframeTo(float searchTime, float targetTime);

    Ogre::Skeleton* mSkeleton;
    std::string     mAnimationName;
    std::string     mBoneName;
    float           mOldTime;
    float           mNewTime;
};

#endif // MOVE_KEYFRAME_COMMAND_H
