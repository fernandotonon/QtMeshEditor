#include "BrushFootprint.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace BrushFootprint {
namespace {

float clamp01(float v)
{
    return std::clamp(v, 0.0f, 1.0f);
}

float fractPos(float v)
{
    v = v - std::floor(v);
    if (v < 0.0f)
        v += 1.0f;
    return v;
}

float sampleChannel(const ImageRgba& image, int x, int y, int channel)
{
    if (image.empty())
        return 0.0f;
    x = std::clamp(x, 0, image.width - 1);
    y = std::clamp(y, 0, image.height - 1);
    const size_t off =
        (static_cast<size_t>(y) * static_cast<size_t>(image.width) + static_cast<size_t>(x)) * 4u
        + static_cast<size_t>(channel);
    return static_cast<float>(image.pixels[off]) / 255.0f;
}

float sampleAlphaBilinear(const ImageRgba& image, float u, float v)
{
    if (image.empty())
        return 0.0f;
    u = clamp01(u);
    v = clamp01(v);
    const float xf = u * static_cast<float>(image.width - 1);
    const float yf = v * static_cast<float>(image.height - 1);
    const int x0 = static_cast<int>(std::floor(xf));
    const int y0 = static_cast<int>(std::floor(yf));
    const int x1 = std::min(x0 + 1, image.width - 1);
    const int y1 = std::min(y0 + 1, image.height - 1);
    const float tx = xf - static_cast<float>(x0);
    const float ty = yf - static_cast<float>(y0);

    const auto a = [&](int x, int y) {
        const float r = sampleChannel(image, x, y, 0);
        const float g = sampleChannel(image, x, y, 1);
        const float b = sampleChannel(image, x, y, 2);
        const float alpha = sampleChannel(image, x, y, 3);
        if (alpha > 1e-4f)
            return alpha;
        return 0.299f * r + 0.587f * g + 0.114f * b;
    };

    const float v00 = a(x0, y0);
    const float v10 = a(x1, y0);
    const float v01 = a(x0, y1);
    const float v11 = a(x1, y1);
    const float top = v00 + (v10 - v00) * tx;
    const float bot = v01 + (v11 - v01) * tx;
    return top + (bot - top) * ty;
}

GradientRamp::Rgba sampleRgbBilinear(const ImageRgba& image, float u, float v)
{
    if (image.empty())
        return {};
    u = fractPos(u);
    v = fractPos(v);
    const float xf = u * static_cast<float>(image.width);
    const float yf = v * static_cast<float>(image.height);
    const int x0 = static_cast<int>(std::floor(xf)) % image.width;
    const int y0 = static_cast<int>(std::floor(yf)) % image.height;
    const int x1 = (x0 + 1) % image.width;
    const int y1 = (y0 + 1) % image.height;
    const float tx = xf - std::floor(xf);
    const float ty = yf - std::floor(yf);

    const auto px = [&](int x, int y) {
        return GradientRamp::Rgba{
            sampleChannel(image, x, y, 0),
            sampleChannel(image, x, y, 1),
            sampleChannel(image, x, y, 2),
            sampleChannel(image, x, y, 3),
        };
    };
    const GradientRamp::Rgba c00 = px(x0, y0);
    const GradientRamp::Rgba c10 = px(x1, y0);
    const GradientRamp::Rgba c01 = px(x0, y1);
    const GradientRamp::Rgba c11 = px(x1, y1);
    const auto lerp = [](const GradientRamp::Rgba& a, const GradientRamp::Rgba& b, float t) {
        return GradientRamp::Rgba{
            a.r + (b.r - a.r) * t,
            a.g + (b.g - a.g) * t,
            a.b + (b.b - a.b) * t,
            a.a + (b.a - a.a) * t,
        };
    };
    const GradientRamp::Rgba top = lerp(c00, c10, tx);
    const GradientRamp::Rgba bot = lerp(c01, c11, tx);
    return lerp(top, bot, ty);
}

} // namespace

float degToRad(float deg)
{
    return deg * static_cast<float>(M_PI) / 180.0f;
}

float stampSpacingUv(float radiusUv, float spacingFraction)
{
    spacingFraction = std::max(spacingFraction, 0.05f);
    return std::max(radiusUv * spacingFraction, 0.002f);
}

void applyScatter(float radiusUv, float scatterFraction, float randU, float randV,
                  float& outDu, float& outDv)
{
    if (scatterFraction <= 0.0f) {
        outDu = outDv = 0.0f;
        return;
    }
    const float amp = radiusUv * scatterFraction;
    outDu = (randU * 2.0f - 1.0f) * amp;
    outDv = (randV * 2.0f - 1.0f) * amp;
}

float stampRotationRad(const StampSettings& settings, float strokeDirRad,
                       float rand01)
{
    switch (settings.rotation) {
    case StampRotation::None:
        return 0.0f;
    case StampRotation::Fixed:
        return degToRad(settings.fixedAngleDeg);
    case StampRotation::StrokeDirection:
        return strokeDirRad;
    case StampRotation::RandomJitter:
        return degToRad((rand01 * 2.0f - 1.0f) * settings.rotationJitterDeg);
    }
    return 0.0f;
}

float jitteredRadius(float radiusUv, float sizeJitter, float rand01)
{
    if (sizeJitter <= 0.0f)
        return radiusUv;
    const float scale = 1.0f + (rand01 * 2.0f - 1.0f) * sizeJitter;
    return std::max(radiusUv * scale, 0.001f);
}

float jitteredStrength(float strength, float opacityJitter, float rand01)
{
    if (opacityJitter <= 0.0f)
        return strength;
    const float scale = 1.0f + (rand01 * 2.0f - 1.0f) * opacityJitter;
    return std::clamp(strength * scale, 0.0f, 1.0f);
}

float sampleStampAlpha01(const ImageRgba& image, float u, float v)
{
    return sampleAlphaBilinear(image, u, v);
}

float sampleStampAlpha(const ImageRgba& image, float dx, float dy)
{
    const float u = (dx + 1.0f) * 0.5f;
    const float v = (dy + 1.0f) * 0.5f;
    return sampleAlphaBilinear(image, u, v);
}

RasterizedStamp rasterizeStamp(const ImageRgba& image, int pixelSize)
{
    RasterizedStamp out;
    if (image.empty() || pixelSize <= 0)
        return out;
    out.size = pixelSize;
    out.alpha.assign(static_cast<size_t>(pixelSize) * static_cast<size_t>(pixelSize), 0.0f);
    for (int y = 0; y < pixelSize; ++y) {
        for (int x = 0; x < pixelSize; ++x) {
            const float dx = (static_cast<float>(x) + 0.5f) / static_cast<float>(pixelSize) * 2.0f - 1.0f;
            const float dy = (static_cast<float>(y) + 0.5f) / static_cast<float>(pixelSize) * 2.0f - 1.0f;
            out.alpha[static_cast<size_t>(y) * static_cast<size_t>(pixelSize) + static_cast<size_t>(x)] =
                sampleStampAlpha(image, dx, dy);
        }
    }
    return out;
}

GradientRamp::Rgba sampleTiling(const ImageRgba& tiling, float u, float v,
                                const TilingSettings& settings)
{
    if (tiling.empty())
        return {1, 1, 1, 1};
    float tu = (u + settings.offsetU) * settings.scale;
    float tv = (v + settings.offsetV) * settings.scale;
    if (settings.rotationDeg != 0.0f) {
        const float rad = degToRad(settings.rotationDeg);
        const float c = std::cos(rad);
        const float s = std::sin(rad);
        const float cx = tu - 0.5f;
        const float cy = tv - 0.5f;
        tu = cx * c - cy * s + 0.5f;
        tv = cx * s + cy * c + 0.5f;
    }
    return sampleRgbBilinear(tiling, tu, tv);
}

void brushOffsetToUv(float centerU, float centerV, float radiusUv, float dx,
                     float dy, float& outU, float& outV)
{
    outU = centerU + dx * radiusUv;
    outV = centerV + dy * radiusUv;
}

} // namespace BrushFootprint
