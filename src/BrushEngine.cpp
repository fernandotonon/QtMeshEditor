#include "BrushEngine.h"

#include <cmath>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace BrushEngine {
namespace {

/// Wrap into [0,1], preserving the closed endpoint so sample(1) hits the
/// last ramp stop. Values outside [0,1] fold via floor (phase jitter /
/// multi-cycle strokeT).
float wrap01(float t)
{
    if (t >= 0.0f && t <= 1.0f)
        return t;
    t = t - std::floor(t);
    if (t < 0.0f)
        t += 1.0f;
    // float noise can land exactly on 1 after the floor of a near-integer
    if (t >= 1.0f)
        return 0.0f;
    return t;
}

} // namespace

float linearStrokeT(float pathLength, float wavelength, float phase)
{
    if (!(wavelength > 1e-8f))
        return wrap01(phase);
    // Exact cycle boundaries map back to 0 so a repeating ramp is seamless.
    float t = pathLength / wavelength + phase;
    t = t - std::floor(t);
    if (t < 0.0f)
        t += 1.0f;
    if (t >= 1.0f - 1e-6f)
        return 0.0f;
    return t;
}

float radialT(float dx, float dy)
{
    const float r = std::sqrt(dx * dx + dy * dy);
    if (r <= 0.0f)
        return 0.0f;
    if (r >= 1.0f)
        return 1.0f;
    return r;
}

float angularT(float dx, float dy)
{
    if (dx == 0.0f && dy == 0.0f)
        return 0.0f;
    float a = std::atan2(dy, dx); // (−π, π]
    a = a / (2.0f * static_cast<float>(M_PI)); // (−0.5, 0.5]
    if (a < 0.0f)
        a += 1.0f;
    return a;
}

GradientRamp::Rgba sampleColor(const SampleParams& p)
{
    if (p.source != ColorSource::Gradient || !p.ramp || !p.ramp->isValid())
        return p.solid;

    float t = 0.0f;
    switch (p.mode) {
    case GradientMode::Linear:
        t = wrap01(p.strokeT + p.phaseJitter);
        break;
    case GradientMode::Radial:
        t = wrap01(radialT(p.dx, p.dy) + p.phaseJitter);
        break;
    case GradientMode::Angular:
        t = wrap01(angularT(p.dx, p.dy) + p.phaseJitter);
        break;
    }
    return p.ramp->sample(t);
}

} // namespace BrushEngine
