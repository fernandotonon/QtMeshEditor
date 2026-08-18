#ifndef FACECAPGEOM_H
#define FACECAPGEOM_H

// Image-space geometry for the mocap predictors (epic #869, Slice C #872):
// letterbox transform + inverse, SSD anchor generation + detection decode +
// weighted NMS, detection->ROI rects, rotated-rect crop sampling and landmark
// re-projection. Every function mirrors the MediaPipe calculator semantics
// proven in scripts/export-facecap-onnx.py — see docs/MOCAP_SPIKE.md for the
// contract (incl. the "plain bilinear, never antialiased" requirement).
// QtGui-only (QImage); no Ogre/ONNX; headless-tested.

#include <QImage>

#include <array>
#include <vector>

namespace FaceCapGeom {

// ---- letterbox --------------------------------------------------------------

struct Letterbox {
    float padX = 0.f;   // normalized padding left of the image content
    float padY = 0.f;   // normalized padding above the image content
    float fracX = 1.f;  // fraction of the square the content occupies in x
    float fracY = 1.f;
};

// Keep-aspect centre letterbox of an RGB888 image into a size x size float
// tensor, NHWC interleaved RGB, plain bilinear, zero border. Values are
// normalized to [lo, hi] ((v/255) * (hi-lo) + lo). out must hold
// size*size*3 floats.
Letterbox letterboxToTensor(const QImage& rgb888, int size, float lo, float hi,
                            float* out);

// Map a point from letterboxed-square normalized coords back to original
// normalized image coords.
inline void unletterbox(const Letterbox& lb, float& x, float& y)
{
    x = (x - lb.padX) / lb.fracX;
    y = (y - lb.padY) / lb.fracY;
}

// ---- SSD detection decode ---------------------------------------------------

struct Detection {
    float score = 0.f;
    // box in normalized coords of the DECODE frame (the letterboxed square
    // until unletterboxed): x, y = top-left; w, h.
    std::array<float, 4> box{0.f, 0.f, 0.f, 0.f};
    std::vector<std::array<float, 2>> keypoints;
};

// MediaPipe SsdAnchorsCalculator with aspect_ratios=[1.0], fixed anchor size,
// min/max scale 0.1484375/0.75, offset 0.5, interpolated aspect 1.0.
// Face detector: inputSize 128, strides {8,16,16,16} -> 896 anchors.
// Pose detector: inputSize 224, strides {8,16,32,32,32} -> 2254 anchors.
std::vector<std::array<float, 2>> genSsdAnchors(int inputSize,
                                                const std::vector<int>& strides);

// TensorsToDetections + weighted NMS. rawBoxes is anchorCount x boxDim
// (boxDim = 4 + 2*numKeypoints), rawScores anchorCount x 1 (logits).
std::vector<Detection> decodeDetections(
    const float* rawBoxes, const float* rawScores,
    const std::vector<std::array<float, 2>>& anchors, int inputSize,
    int numKeypoints, float minScore = 0.5f, float iouThreshold = 0.3f,
    bool reverseOutputOrder = false);

// ---- ROI rects --------------------------------------------------------------

struct RotatedRect {
    float cx = 0.f, cy = 0.f;  // centre, px
    float w = 0.f, h = 0.f;    // px
    float angle = 0.f;         // MediaPipe rotation, rad: R = [[c,-s],[s,c]] y-down
};

// Face: rotation from detector keypoint 0 (right eye) -> 1 (left eye), target
// angle 0; box made square-long then scaled 1.5x. Detection coords normalized
// to the ORIGINAL image (unletterbox first).
RotatedRect rectFromFaceDetection(const Detection& det, int imgW, int imgH);

// Tracking mode: next-frame ROI from the previous frame's 478 landmarks (px):
// bounding box -> square-long * 1.5, rotation from eye outer corners 33->263.
RotatedRect rectFromFaceLandmarks(const float* landmarksXyz, int count,
                                  int imgW, int imgH);

// Pose: centre keypoint 0 (mid-hip), radius |kp1-kp0|, box 2*radius square,
// target angle 90 deg, scaled 1.25x.
RotatedRect rectFromPoseDetection(const Detection& det, int imgW, int imgH);

// Hand crop from BlazePose image-space wrist + index + pinky (Holistic-style
// pose-seeded ROI). Fingers are oriented up in the crop (wrist toward +Y).
RotatedRect rectFromPoseHand(float wristX, float wristY, float indexX,
                             float indexY, float pinkyX, float pinkyY);

// MediaPipe BlazePalm → landmark ROI: wrist (kp0) → middle MCP (kp2),
// target 90°, box × 2.6, shift_y −0.5 so fingertips stay in the crop.
RotatedRect rectFromPalmDetection(const Detection& det, int imgW, int imgH);

// Next-frame hand tracking ROI from the previous 21 image-space landmarks
// (wrist → middle MCP, scale 2.0).
RotatedRect rectFromHandLandmarks(const float* imageXy21x2, int imgW, int imgH);

// MediaPipe Hands: 21 landmarks. Flex at B of triangle A-B-C is π − angle
// (0 = colinear/open, ~1.6 = tightly folded).
constexpr int kHandLandmarkCount = 21;
float handJointFlexRad(const float* xyz21, int a, int b, int c);
// outFlex[finger][segment] for thumb..pinky × MCP/PIP/DIP (3 segments).
void handFingerFlexRad(const float* xyz21, float outFlex[5][3]);

// ---- crop + projection ------------------------------------------------------

// Sample the rotated rect into a size x size float tensor (NHWC RGB, range
// [lo, hi], plain bilinear, zero border):
//   p_img = centre + R(angle) . ((u/size - 0.5) * w, (v/size - 0.5) * h)
void cropRotatedRectToTensor(const QImage& rgb888, const RotatedRect& rect,
                             int size, float lo, float hi, float* out);

// LandmarkProjection: landmark (x,y[,z]) normalized to the crop -> image px
// (z scaled by rect.w). pts is count x stride floats, mutated in place.
void projectLandmarks(float* pts, int count, int stride, const RotatedRect& rect);

}  // namespace FaceCapGeom

#endif  // FACECAPGEOM_H
