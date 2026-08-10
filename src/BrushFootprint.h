#ifndef BRUSH_FOOTPRINT_H
#define BRUSH_FOOTPRINT_H

#include "GradientRamp.h"

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

/**
 * @brief Paint v2 Slice B (#545) — stamp / tiling brush footprints.
 *
 * Pure data. No Ogre dependency — TexturePaintController converts sampled
 * colours into Ogre::ColourValue at the stamp site.
 */
namespace BrushFootprint {

enum class FootprintType {
    Round = 0,
    Square = 1,
    StampImage = 2,
    TilingSource = 3,
};

enum class StampRotation {
    None = 0,
    Fixed = 1,
    StrokeDirection = 2,
    RandomJitter = 3,
};

struct StampSettings {
    /// Distance between stamp centres as a fraction of brush radius.
    float spacing = 0.35f;
    /// Random perpendicular offset as a fraction of brush radius.
    float scatter = 0.0f;
    /// Per-stamp radius scale jitter ∈ [0,1].
    float sizeJitter = 0.0f;
    /// Per-stamp strength scale jitter ∈ [0,1].
    float opacityJitter = 0.0f;
    StampRotation rotation = StampRotation::None;
    float fixedAngleDeg = 0.0f;
    float rotationJitterDeg = 15.0f;
};

struct TilingSettings {
    float scale = 1.0f;
    float rotationDeg = 0.0f;
    float offsetU = 0.0f;
    float offsetV = 0.0f;
};

/// RGBA8 image in row-major top-left order.
struct ImageRgba {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> pixels; ///< size = w*h*4

    bool empty() const { return width <= 0 || height <= 0 || pixels.empty(); }
};

/// Pre-rasterised stamp alpha mask in brush space (−1..1 square grid).
struct RasterizedStamp {
    int size = 0; ///< square side in texels
    std::vector<float> alpha;     ///< [0,1], row-major

    bool empty() const { return size <= 0 || alpha.empty(); }
};

float degToRad(float deg);

/// Stamp dab spacing in UV units.
float stampSpacingUv(float radiusUv, float spacingFraction);

/// Perpendicular scatter offset in UV space.
void applyScatter(float radiusUv, float scatterFraction, float randU, float randV,
                  float& outDu, float& outDv);

/// Resolve stamp rotation in radians.
float stampRotationRad(const StampSettings& settings, float strokeDirRad,
                       float rand01);

/// Scale radius by size jitter draw.
float jitteredRadius(float radiusUv, float sizeJitter, float rand01);

/// Scale strength by opacity jitter draw.
float jitteredStrength(float strength, float opacityJitter, float rand01);

/// Bilinear sample of stamp alpha in normalised stamp coords ∈ [0,1]².
float sampleStampAlpha01(const ImageRgba& image, float u, float v);

/// Bilinear sample of stamp alpha in brush space (−1..1).
float sampleStampAlpha(const ImageRgba& image, float dx, float dy);

/// Rasterise a stamp image into a square alpha grid for fast stamping.
RasterizedStamp rasterizeStamp(const ImageRgba& image, int pixelSize);

/// Sample a tileable source at absolute UV with scale / rotation / offset.
GradientRamp::Rgba sampleTiling(const ImageRgba& tiling, float u, float v,
                                const TilingSettings& settings);

/// Map brush-space (−1..1) to absolute UV around a stamp centre.
void brushOffsetToUv(float centerU, float centerV, float radiusUv, float dx,
                     float dy, float& outU, float& outV);

} // namespace BrushFootprint

#endif // BRUSH_FOOTPRINT_H
