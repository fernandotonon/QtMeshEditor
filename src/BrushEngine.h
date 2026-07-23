#ifndef BRUSH_ENGINE_H
#define BRUSH_ENGINE_H

#include "GradientRamp.h"

/**
 * @brief Paint v2 BrushEngine — colour-source sampling for brush stamps.
 *
 * Slice A (#544) extracts the colour decision out of TexturePaintController's
 * stamp loop so solid and gradient sources compose cleanly. Later slices
 * (textured stamps, cavity masks, pressure) plug into the same SampleParams
 * without re-touching every tool.
 *
 * Pure data. No Qt / Ogre dependency — TexturePaintController converts the
 * returned Rgba into Ogre::ColourValue at the stamp site.
 */
namespace BrushEngine {

enum class ColorSource {
    Solid = 0,
    Gradient = 1,
};

/// How a gradient maps onto a stamp / stroke.
enum class GradientMode {
    /// Colour advances with stroke path length (`strokeT`).
    Linear = 0,
    /// Colour radiates from brush centre → edge (`dx`,`dy` length).
    Radial = 1,
    /// Colour cycles around the brush centre (atan2 of `dx`,`dy`).
    Angular = 2,
};

struct SampleParams {
    ColorSource source = ColorSource::Solid;
    GradientRamp::Rgba solid{};
    const GradientRamp::Ramp* ramp = nullptr;
    GradientMode mode = GradientMode::Linear;
    /// Linear-mode parameter ∈ [0,1] (typically fract(pathLen / wavelength)).
    float strokeT = 0.0f;
    /// Optional phase offset added before sampling (random ramp jitter).
    float phaseJitter = 0.0f;
    /// Normalised offset from brush centre in stamp space (−1..1).
    /// Used by Radial / Angular; ignored by Solid and Linear.
    float dx = 0.0f;
    float dy = 0.0f;
};

/// Map stroke path length into a repeating [0,1] parameter.
/// `wavelength` is the UV-space distance that covers one full ramp cycle
/// (defaults to ~4× brush radius in the controller).
float linearStrokeT(float pathLength, float wavelength, float phase = 0.0f);

/// Radial t = clamp(length(dx,dy), 0..1).
float radialT(float dx, float dy);

/// Angular t = atan2(dy,dx) mapped into [0,1).
float angularT(float dx, float dy);

/// Sample the active colour source. Falls back to `solid` when the ramp
/// is missing / invalid.
GradientRamp::Rgba sampleColor(const SampleParams& p);

} // namespace BrushEngine

#endif // BRUSH_ENGINE_H
