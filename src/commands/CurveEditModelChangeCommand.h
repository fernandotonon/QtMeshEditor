#ifndef CURVE_EDIT_MODEL_CHANGE_COMMAND_H
#define CURVE_EDIT_MODEL_CHANGE_COMMAND_H

#include <QUndoCommand>
#include <string>

/**
 * Captures one keyframe entry's CurveEditModel state (in/out tangent + mode)
 * before a change so undo can restore it. Pairs with ResampleCurveCommand
 * inside a QUndoStack macro: the macro pushes the side-table change, then
 * the resample(s), so a single Ctrl+Z restores both the model entry and
 * the underlying TransformKeyFrames.
 */
class CurveEditModelChangeCommand : public QUndoCommand
{
public:
    /// `oldMode` / `newMode` are CurveEditModel::InterpMode values cast to int.
    CurveEditModelChangeCommand(std::string skeleton,
                                std::string animation,
                                std::string bone,
                                std::string channel,
                                double time,
                                double oldIn,  double oldOut,  int oldMode,
                                double newIn,  double newOut,  int newMode,
                                QUndoCommand* parent = nullptr);

    void undo() override;
    void redo() override;

private:
    void apply(double in, double out, int mode);

    std::string mSkeleton;
    std::string mAnimation;
    std::string mBone;
    std::string mChannel;
    double      mTime;
    double      mOldIn, mOldOut;
    int         mOldMode;
    double      mNewIn, mNewOut;
    int         mNewMode;
};

#endif // CURVE_EDIT_MODEL_CHANGE_COMMAND_H
