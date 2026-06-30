#include "HDR/HdrTonemap.h"

#include <algorithm>

namespace HdrTonemap {
namespace {

Rgb saturate(Rgb c)
{
    return clamp01(c);
}

} // namespace

Rgb applyExposure(Rgb linear, float exposureEv)
{
    return linear * exposureMultiplier(exposureEv);
}

Rgb tonemapReinhard(Rgb linear, float whitePoint)
{
    const float wp = std::max(0.001f, whitePoint);
    const float invWp2 = 1.f / (wp * wp);
    Rgb mapped = {
        linear.r * (1.f + linear.r * invWp2) / (1.f + linear.r),
        linear.g * (1.f + linear.g * invWp2) / (1.f + linear.g),
        linear.b * (1.f + linear.b * invWp2) / (1.f + linear.b),
    };
    return saturate(mapped);
}

Rgb tonemapAces(Rgb x)
{
    constexpr float a = 2.51f;
    constexpr float b = 0.03f;
    constexpr float c = 2.43f;
    constexpr float d = 0.59f;
    constexpr float e = 0.14f;
    auto film = [&](float v) {
        const float num = v * (a * v + b);
        const float den = v * (c * v + d) + e;
        return clamp01(num / den);
    };
    return {film(x.r), film(x.g), film(x.b)};
}

Rgb tonemapAgx(Rgb x)
{
    // Troy Sobotka's AgX default contrast approximation (Blender 4+ default).
    const Rgb lin = {
        std::max(0.f, x.r),
        std::max(0.f, x.g),
        std::max(0.f, x.b),
    };
    const Rgb agx = {
        0.224282f * lin.r + 0.130789f * lin.g + 0.044929f * lin.b,
        0.050223f * lin.r + 0.873461f * lin.g + 0.076316f * lin.b,
        0.020833f * lin.r + 0.080745f * lin.g + 0.898422f * lin.b,
    };
    const Rgb contrast = {
        std::pow(std::max(0.f, agx.r), 1.35f),
        std::pow(std::max(0.f, agx.g), 1.35f),
        std::pow(std::max(0.f, agx.b), 1.35f),
    };
    return saturate(contrast);
}

Rgb tonemap(Rgb linearHdr, Operator op, float exposureEv, float whitePoint)
{
    Rgb exposed = applyExposure(linearHdr, exposureEv);
    switch (op) {
    case Operator::ACES:
        return tonemapAces(exposed);
    case Operator::AgX:
        return tonemapAgx(exposed);
    case Operator::Reinhard:
    default:
        return tonemapReinhard(exposed, whitePoint);
    }
}

Rgb linearToSrgb(Rgb linear)
{
    auto encode = [](float c) {
        c = std::max(0.f, c);
        if (c <= 0.0031308f)
            return 12.92f * c;
        return 1.055f * std::pow(c, 1.f / 2.4f) - 0.055f;
    };
    return {encode(linear.r), encode(linear.g), encode(linear.b)};
}

} // namespace HdrTonemap
