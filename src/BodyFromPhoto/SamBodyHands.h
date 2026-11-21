#pragma once
#include "SamBodyPredictor.h"

#include <QPointF>
#include <QRectF>

#include <array>
#include <vector>

/// Hand refinement for SAM 3D Body: the pure-data half of Meta's `run_inference(..., inference_type="full")`.
///
/// The body pass predicts the fingers from the body crop, where a hand is a few dozen pixels. Meta's full pipeline then
///   1. takes the two hand boxes the body decoder predicts (hand-box tokens),
///   2. runs the backbone + a dedicated HAND decoder on each hand crop (the LEFT hand on the mirrored photo: the hand
///      decoder only knows right hands),
///   3. keeps a hand only when four tests pass (wrist within 1.4 rad of the body's, box larger than 64 px, every hand
///      keypoint inside its crop, hand wrist within a quarter box of the body wrist),
///   4. re-runs the body decoder with the accepted wrists + the elbows as keypoint PROMPTS,
///   5. drops the hand poses, hand scales and the wrist rotations into the body (`sam3d_hand_fuse.onnx`).
/// Everything here is geometry and bookkeeping: unit-tested without ONNX. Constants are Meta's.
namespace SamBody::Hands {

inline constexpr double kHandPadding = 0.9;     ///< GetBBoxCenterScale(padding=0.9) of the hand transform
inline constexpr double kMinHandBox = 64.0;     ///< criterion 2: crop edge (photo px) must exceed this
inline constexpr double kKeypointLimit = 0.5;   ///< criterion 3: |crop-normalised keypoint| below this
inline constexpr double kWristDistance = 0.25;  ///< criterion 4: wrist offset / box edge below this
inline constexpr int kRightWristKp = 41;
inline constexpr int kLeftWristKp = 62;
inline constexpr int kRightElbowKp = 8;
inline constexpr int kLeftElbowKp = 7;
inline constexpr int kKeypoints = 70;

/// One decoder hand box (cx, cy, w, h in 0..1 of the BODY crop) -> the square photo box Meta crops the hand from
/// (`_get_hand_box`: edge = max(w, h), mapped back through the body crop's affine). Empty when not finite.
QRectF handBoxInPhoto(const CropGeometry& body, const std::array<float, 4>& box);

/// Horizontal mirror of a photo box / point (Meta: x' = W - x - 1).
QRectF mirrorBox(const QRectF& box, int photoW);
QPointF mirrorPoint(const QPointF& p, int photoW);

/// Photo px -> body crop, normalised to [-0.5, 0.5] (Meta's `_full_to_crop`).
QPointF photoToCrop(const CropGeometry& geo, const QPointF& p);

/// Criterion 3: every crop-normalised keypoint (x,y pairs) strictly inside +-kKeypointLimit.
bool keypointsInside(const std::vector<float>& kp2dCropped);

/// Keypoint `index` of a flat (x,y) list, or a NaN point when out of range.
QPointF keypoint(const std::vector<float>& kp2d, int index);

/// Criterion 4. `handWrist` / `bodyWrist` in photo px; `boxEdge` = the crop edge Meta divides by. NB Meta divides the
/// RIGHT hand's offset by the LEFT crop's edge and vice versa; callers keep that pairing for parity.
bool wristClose(const QPointF& handWrist, const QPointF& bodyWrist, double boxEdge);

struct Prompt { float x = 0, y = 0, label = -2; };   ///< x, y in 0..1 of the body crop

/// The keypoint prompts of step 4, in Meta's order (right wrist, left wrist, right elbow, left elbow): a prompt is kept
/// when its point lies inside the body crop and its side's hand was accepted (`valid` = {left, right}). Coordinates are
/// shifted to 0..1 and clamped. Empty = nothing to re-run.
std::vector<Prompt> wristElbowPrompts(const CropGeometry& body, const QPointF& rightWrist, const QPointF& leftWrist,
                                      const QPointF& rightElbow, const QPointF& leftElbow, const std::array<bool, 2>& valid);

} // namespace SamBody::Hands
