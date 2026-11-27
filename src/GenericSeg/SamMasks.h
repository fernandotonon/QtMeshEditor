#pragma once
// Automatic mask generation post-processing for SAM 2.1 (#1179), the part of
// Meta's SAM2AutomaticMaskGenerator we need, reimplemented: prompt grid on
// the foreground, per-mask quality filters, mask NMS, upsampling. Pure data
// (no ONNX), so it is unit-tested; SamMaskProvider feeds it decoder output.

#include "MeshRaster.h"

#include <array>
#include <cstdint>
#include <vector>

namespace GenericSeg::Sam {

constexpr int kInputSize = 1024;   // encoder input edge
constexpr int kLowRes = 256;       // decoder mask edge

// Prompt points: a perSide x perSide grid over the image, keeping only cells
// whose centre pixel shows the mesh. Coordinates are pixels of the 1024 input
// frame (x right, y down), as the decoder graph expects.
std::vector<std::array<float, 2>> gridPoints(const Raster& r, int perSide);

// SAM's stability score: IoU between the mask thresholded at +offset and at
// -offset. Logits are kLowRes^2.
float stabilityScore(const float* logits, int count, float offset);

struct Candidate {
    float score = 0.0f;          // predicted IoU
    float stability = 0.0f;
    std::vector<uint8_t> low;    // kLowRes^2 binary mask (logit > 0)
    int area = 0;                // pixels set in `low`
    std::array<int, 4> box{};    // x0, y0, x1, y1 inclusive, in `low`
    const float* logits = nullptr;   // owner's buffer (valid until upsampling)
};

// Fill low/area/box from logits; returns false for an empty mask.
bool binarize(Candidate& c);

// Greedy NMS by descending score: drop a candidate whose mask IoU with an
// already kept one exceeds `iouThreshold`. Returns kept indices in order.
std::vector<int> nms(const std::vector<Candidate>& candidates, float iouThreshold);

// Bilinear (align_corners = false, like F.interpolate) resize of kLowRes^2
// logits to size^2, thresholded at 0.
std::vector<uint8_t> upsample(const float* logits, int size);

struct FilterOptions {
    float minPredictedIou = 0.7f;
    float minStability = 0.85f;
    float stabilityOffset = 1.0f;
    float nmsIou = 0.7f;
    float minAreaFraction = 0.001f;   // of the foreground pixels
};

} // namespace GenericSeg::Sam
