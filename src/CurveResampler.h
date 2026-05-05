#ifndef CURVE_RESAMPLER_H
#define CURVE_RESAMPLER_H

#include <QString>
#include <QVariantList>
#include <vector>

class CurveEditModel;

/**
 * Walks a curve segment defined by CurveEditModel tangents/modes and
 * produces a dense set of (time, value) samples for one channel. The
 * caller writes those samples back into Ogre's TransformKeyFrames so
 * playback follows the visual curve.
 *
 * Sample rate: 30 Hz baseline, bumped to 60 Hz over high-curvature
 * windows (peak-detected from the second derivative). The output is
 * capped at 200 keyframes per segment so a long animation can't blow
 * up the track size.
 *
 * Pure-data — no Ogre dependency. Tested in CurveResampler_test.cpp.
 */
namespace CurveResampler {

struct Sample {
    double time;
    double value;
};

constexpr int    kBaseHz       = 30;
constexpr int    kBoostHz      = 60;
constexpr int    kMaxSamples   = 200;
constexpr double kCurvatureEps = 1.0;
// Simplification tolerance: 0.5% of the segment's value range, with
// a 1e-3 absolute floor for flat-ish curves. Linear segments collapse
// to zero new keyframes (Ogre's default linear interp matches the
// curve); only Stepped/sharp shapes retain dense samples.
constexpr double kSimplifyRel   = 0.005;
constexpr double kSimplifyFloor = 1e-3;

/// Resample one channel between adjacent keyframes [t0, t1].
/// `kfTimes` and `kfValues` are the channel's existing data (must be
/// the same length, in keyframe order). `model` holds tangent/mode
/// state. Returns the new samples in (start, end] — the start keyframe
/// is preserved by the caller, and the end keyframe is included so the
/// segment closes cleanly. Empty when the inputs are invalid.
std::vector<Sample> resampleSegment(const CurveEditModel* model,
                                    const QString& skeleton,
                                    const QString& anim,
                                    const QString& bone,
                                    const QString& channel,
                                    double t0, double t1,
                                    const QVariantList& kfTimes,
                                    const QVariantList& kfValues);

} // namespace CurveResampler

#endif // CURVE_RESAMPLER_H
