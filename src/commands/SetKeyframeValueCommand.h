#ifndef SET_KEYFRAME_VALUE_COMMAND_H
#define SET_KEYFRAME_VALUE_COMMAND_H

#include <QUndoCommand>
#include <string>

namespace Ogre {
    class Skeleton;
}

/**
 * Sets a single TRS channel (tx/ty/tz/rw/rx/ry/rz/sx/sy/sz) on a specific
 * keyframe of a bone's track, leaving the other nine channels untouched.
 * Used by the curve editor when the user drags a keyframe square in the
 * Y axis (channel value) without retiming.
 *
 * Stores the previous value so undo restores it exactly. Stores the
 * channel name as a string so the command survives any track rebuilds
 * that happen between push and undo.
 */
class SetKeyframeValueCommand : public QUndoCommand
{
public:
    SetKeyframeValueCommand(Ogre::Skeleton* skeleton,
                            std::string animationName,
                            std::string boneName,
                            std::string channel,
                            float time,
                            double newValue,
                            QUndoCommand* parent = nullptr);

    void undo() override;
    void redo() override;

private:
    bool apply(double value);

    Ogre::Skeleton* mSkeleton;
    std::string     mAnimationName;
    std::string     mBoneName;
    std::string     mChannel;
    float           mTime;
    double          mOldValue = 0.0;
    double          mNewValue;
    bool            mCaptured = false;
};

#endif // SET_KEYFRAME_VALUE_COMMAND_H
