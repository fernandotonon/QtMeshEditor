#ifndef RESAMPLE_CURVE_COMMAND_H
#define RESAMPLE_CURVE_COMMAND_H

#include <QUndoCommand>
#include <OgreVector3.h>
#include <OgreQuaternion.h>
#include <string>
#include <vector>

namespace Ogre { class TransformKeyFrame; }

/**
 * Resamples a curve segment (between two adjacent keyframes) into a
 * dense set of TransformKeyFrames so live Ogre playback follows the
 * Bezier/Auto/Stepped/Linear shape held in CurveEditModel. Pushed once
 * per gesture (tangent drag release, value drag release, mode change)
 * so a single Ctrl+Z reverts the whole edit.
 *
 * Captures every keyframe inside (t0, t1] before the first redo so
 * undo can restore the pre-resample state exactly. New keyframes get
 * non-resampled channels filled by linearly interpolating the bracketing
 * keyframes' TRS — that preserves the other 9 channels' shape between
 * the two anchors.
 */
class ResampleCurveCommand : public QUndoCommand
{
public:
    ResampleCurveCommand(std::string entityName,
                         std::string animationName,
                         std::string boneName,
                         std::string channel,
                         float t0, float t1,
                         double toleranceMul = 1.0,
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
    bool captureBefore();
    bool applySnapshot(const std::vector<KeyframeSnapshot>& snap);
    bool resampleAndWrite();

    std::string mEntityName;
    std::string mAnimationName;
    std::string mBoneName;
    std::string mChannel;
    float       mT0;
    float       mT1;
    double      mToleranceMul;
    std::vector<KeyframeSnapshot> mBefore;
    std::vector<KeyframeSnapshot> mAfter;
    bool        mCaptured = false;
};

#endif // RESAMPLE_CURVE_COMMAND_H
