#pragma once

#include "SamBodyPredictor.h"

#include <QImage>
#include <QString>

#include <functional>
#include <vector>

/// #1100: SAM 3D Body over a SEQUENCE of images (video frames, recorded camera frames, pose photos): one MHR pose per image,
/// the sessions loaded once. The result is plain data (no Ogre) that `retargetSequenceToSkeleton` (SamBodyRetarget.h) writes onto a
/// rig, so the CLI, the GUI (camera / video / pose images) and MCP all share it.
namespace SamBody {

struct MhrSequence {
    std::vector<std::vector<float>> jointQuat;   ///< per frame 127*4 GLOBAL rotations (x,y,z,w)
    std::vector<std::vector<float>> jointPos;    ///< per frame 127*3 positions, metres
    std::vector<double> times;                   ///< seconds per frame; a dropout keeps its slot (held pose), so timing is preserved
    std::vector<float> restPos, restQuat;        ///< the MHR rest pose (all-zero parameters)
    int solved = 0;                              ///< frames SAM actually solved
    int dropouts = 0;                            ///< frames with no person / a failed inference (held, or skipped before the first solve)
    QString error;                               ///< non-empty = nothing usable
    bool ok() const { return error.isEmpty() && solved >= 2; }
};

/// (frames done, total, what) -> false to cancel.
using SequenceProgress = std::function<bool(int done, int total, const QString& what)>;

/// Run SAM over `images` (same length as `times`, ascending seconds). `box` (photo pixels, optional) pins the person box for every
/// frame; empty = find the person per frame. Never throws; `error` explains an unusable result (cancelled, no ONNX build, too few
/// people found).
MhrSequence solveSequence(Runner& runner, const std::vector<QImage>& images, const std::vector<double>& times,
                          const SequenceProgress& progress = {}, const QRectF& box = {});

/// Frames are kept at a capture-friendly size: the model crops to 512 px anyway, and a minute of 1080p frames would not fit in memory.
/// The camera intrinsics are derived from the image size, so a proportional downscale keeps them consistent.
QImage downscaledForCapture(const QImage& image, int maxEdge = 1280);

} // namespace SamBody
