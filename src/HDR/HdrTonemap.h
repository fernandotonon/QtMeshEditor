#pragma once

#include <algorithm>
#include <cmath>

namespace HdrTonemap {

enum class Operator {
    Reinhard = 0,
    ACES = 1,
    AgX = 2,
};

struct Rgb {
    float r = 0.f;
    float g = 0.f;
    float b = 0.f;
};

inline Rgb operator*(Rgb c, float s)
{
    return {c.r * s, c.g * s, c.b * s};
}

inline Rgb operator+(Rgb a, Rgb b)
{
    return {a.r + b.r, a.g + b.g, a.b + b.b};
}

inline Rgb operator/(Rgb c, float s)
{
    return {c.r / s, c.g / s, c.b / s};
}

inline float clamp01(float v)
{
    return std::max(0.f, std::min(1.f, v));
}

inline Rgb clamp01(Rgb c)
{
    return {clamp01(c.r), clamp01(c.g), clamp01(c.b)};
}

/// Exposure in EV stops: linear scale = 2^exposureEv.
inline float exposureMultiplier(float exposureEv)
{
    return std::pow(2.f, exposureEv);
}

Rgb applyExposure(Rgb linear, float exposureEv);
Rgb tonemapReinhard(Rgb linear, float whitePoint);
Rgb tonemapAces(Rgb linear);
Rgb tonemapAgx(Rgb linear);
Rgb tonemap(Rgb linearHdr, Operator op, float exposureEv, float whitePoint);
Rgb linearToSrgb(Rgb linear);

} // namespace HdrTonemap
