#include "PaintLayerBlend.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace PaintLayerBlend {
namespace {

constexpr float kEps = 1e-6f;

float clamp01(float v) { return std::clamp(v, 0.f, 1.f); }

struct Hsl {
    float h = 0.f;
    float s = 0.f;
    float l = 0.f;
};

Hsl rgbToHsl(float r, float g, float b)
{
    const float maxC = std::max({r, g, b});
    const float minC = std::min({r, g, b});
    const float l = (maxC + minC) * 0.5f;
    if (maxC - minC < kEps) return {0.f, 0.f, l};

    const float d = maxC - minC;
    float h = 0.f;
    if (maxC == r)
        h = std::fmod((g - b) / d + (g < b ? 6.f : 0.f), 6.f);
    else if (maxC == g)
        h = (b - r) / d + 2.f;
    else
        h = (r - g) / d + 4.f;
    h /= 6.f;

    const float s = d / (1.f - std::abs(2.f * l - 1.f));
    return {h, s, l};
}

float hueToRgb(float p, float q, float t)
{
    if (t < 0.f) t += 1.f;
    if (t > 1.f) t -= 1.f;
    if (t < 1.f / 6.f) return p + (q - p) * 6.f * t;
    if (t < 1.f / 2.f) return q;
    if (t < 2.f / 3.f) return p + (q - p) * (2.f / 3.f - t) * 6.f;
    return p;
}

Rgba hslToRgb(float h, float s, float l)
{
    if (s < kEps) return {l, l, l, 1.f};
    const float q = l < 0.5f ? l * (1.f + s) : l + s - l * s;
    const float p = 2.f * l - q;
    return {
        hueToRgb(p, q, h + 1.f / 3.f),
        hueToRgb(p, q, h),
        hueToRgb(p, q, h - 1.f / 3.f),
        1.f,
    };
}

Rgba blendChannelMode(const Rgba& dst, const Rgba& src, Mode mode)
{
    switch (mode) {
    case Mode::Multiply:
        return {dst.r * src.r, dst.g * src.g, dst.b * src.b, src.a};
    case Mode::Screen:
        return {1.f - (1.f - dst.r) * (1.f - src.r),
                1.f - (1.f - dst.g) * (1.f - src.g),
                1.f - (1.f - dst.b) * (1.f - src.b),
                src.a};
    case Mode::Overlay: {
        auto ov = [](float b, float s) {
            return b < 0.5f ? 2.f * b * s : 1.f - 2.f * (1.f - b) * (1.f - s);
        };
        return {ov(dst.r, src.r), ov(dst.g, src.g), ov(dst.b, src.b), src.a};
    }
    case Mode::Add:
        return {clamp01(dst.r + src.r),
                clamp01(dst.g + src.g),
                clamp01(dst.b + src.b),
                src.a};
    case Mode::Subtract:
        return {clamp01(dst.r - src.r),
                clamp01(dst.g - src.g),
                clamp01(dst.b - src.b),
                src.a};
    case Mode::SoftLight: {
        auto sl = [](float b, float s) {
            return s < 0.5f ? b - (1.f - 2.f * s) * b * (1.f - b)
                            : b + (2.f * s - 1.f) * (std::sqrt(b) - b);
        };
        return {clamp01(sl(dst.r, src.r)),
                clamp01(sl(dst.g, src.g)),
                clamp01(sl(dst.b, src.b)),
                src.a};
    }
    case Mode::Hue: {
        const Hsl dstHsl = rgbToHsl(dst.r, dst.g, dst.b);
        const Hsl srcHsl = rgbToHsl(src.r, src.g, src.b);
        Rgba out = hslToRgb(srcHsl.h, dstHsl.s, dstHsl.l);
        out.a = src.a;
        return out;
    }
    case Mode::Normal:
    default:
        return src;
    }
}

Rgba normalComposite(const Rgba& dst, const Rgba& src, float alpha)
{
    const float a = clamp01(alpha);
    if (a <= kEps) return dst;
    if (a >= 1.f - kEps) return src;
    return {
        src.r * a + dst.r * (1.f - a),
        src.g * a + dst.g * (1.f - a),
        src.b * a + dst.b * (1.f - a),
        a + dst.a * (1.f - a),
    };
}

Rgba compositePixelAt(size_t pixelIndex,
                      const std::vector<LayerInput>& layers)
{
    Rgba acc{1.f, 1.f, 1.f, 1.f};
    bool haveAcc = false;

    for (const auto& layer : layers) {
        if (!layer.visible || !layer.rgba) continue;
        const size_t off = pixelIndex * 4u;
        const uint8_t* px = layer.rgba + off;
        const uint8_t mask = layer.maskAlpha ? layer.maskAlpha[pixelIndex] : 255;
        Rgba src = rgbaFromBytes(px[0], px[1], px[2], px[3]);

        if (!haveAcc) {
            const float a = clamp01(src.a * clamp01(layer.opacity) * byteToF(mask));
            if (a <= kEps)
                continue;
            acc = {src.r, src.g, src.b, a};
            haveAcc = true;
            continue;
        }

        acc = blendPixel(acc, src, layer.blendMode, layer.opacity, mask);
    }

    if (!haveAcc)
        return {0.f, 0.f, 0.f, 0.f};
    return acc;
}

} // namespace

const char* modeName(Mode mode)
{
    switch (mode) {
    case Mode::Normal: return "Normal";
    case Mode::Multiply: return "Multiply";
    case Mode::Screen: return "Screen";
    case Mode::Overlay: return "Overlay";
    case Mode::Add: return "Add";
    case Mode::Subtract: return "Subtract";
    case Mode::SoftLight: return "Soft Light";
    case Mode::Hue: return "Hue";
    }
    return "Normal";
}

Mode modeFromName(const char* name)
{
    if (!name) return Mode::Normal;
    struct Entry { const char* n; Mode m; };
    static const Entry kTable[] = {
        {"Normal", Mode::Normal},
        {"Multiply", Mode::Multiply},
        {"Screen", Mode::Screen},
        {"Overlay", Mode::Overlay},
        {"Add", Mode::Add},
        {"Subtract", Mode::Subtract},
        {"Soft Light", Mode::SoftLight},
        {"SoftLight", Mode::SoftLight},
        {"Hue", Mode::Hue},
    };
    for (const auto& e : kTable) {
        if (std::strcmp(name, e.n) == 0) return e.m;
    }
    return Mode::Normal;
}

Rgba rgbaFromBytes(uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    return {byteToF(r), byteToF(g), byteToF(b), byteToF(a)};
}

void rgbaToBytes(const Rgba& c, uint8_t& r, uint8_t& g, uint8_t& b, uint8_t& a)
{
    r = fToByte(c.r);
    g = fToByte(c.g);
    b = fToByte(c.b);
    a = fToByte(c.a);
}

Rgba blendPixel(const Rgba& dst, const Rgba& src, Mode mode, float opacity, uint8_t mask)
{
    const float maskF = byteToF(mask);
    const float effectiveAlpha = clamp01(src.a * clamp01(opacity) * maskF);
    if (effectiveAlpha <= kEps) return dst;

    Rgba blended = src;
    if (mode != Mode::Normal)
        blended = blendChannelMode(dst, src, mode);

    if (mode == Mode::Normal)
        return normalComposite(dst, blended, effectiveAlpha);

    // Non-normal modes: blend result over dst using effective alpha.
    Rgba out = blended;
    out.a = effectiveAlpha;
    return normalComposite(dst, out, effectiveAlpha);
}

void compositeLayers(int width, int height,
                     const std::vector<LayerInput>& layers,
                     std::vector<uint8_t>& out)
{
    const size_t n = static_cast<size_t>(width) * static_cast<size_t>(height);
    out.assign(n * 4u, 0);
    if (width <= 0 || height <= 0 || layers.empty()) return;

    bool anyVisible = false;
    for (const auto& layer : layers) {
        if (layer.visible && layer.rgba) {
            anyVisible = true;
            break;
        }
    }
    if (!anyVisible) {
        for (size_t i = 0; i < n; ++i) {
            out[i * 4u + 0] = 255;
            out[i * 4u + 1] = 255;
            out[i * 4u + 2] = 255;
            out[i * 4u + 3] = 255;
        }
        return;
    }

    for (size_t i = 0; i < n; ++i) {
        const Rgba acc = compositePixelAt(i, layers);
        if (acc.a <= kEps && acc.r <= kEps && acc.g <= kEps && acc.b <= kEps) {
            out[i * 4u + 0] = 0;
            out[i * 4u + 1] = 0;
            out[i * 4u + 2] = 0;
            out[i * 4u + 3] = 0;
        } else {
            rgbaToBytes(acc, out[i * 4u + 0], out[i * 4u + 1], out[i * 4u + 2], out[i * 4u + 3]);
        }
    }
}

void compositeLayersRegion(int width, int height,
                           const std::vector<LayerInput>& layers,
                           uint8_t* inOut,
                           int x0, int y0, int x1, int y1)
{
    if (!inOut || width <= 0 || height <= 0 || layers.empty()) return;
    x0 = std::clamp(x0, 0, width);
    y0 = std::clamp(y0, 0, height);
    x1 = std::clamp(x1, 0, width);
    y1 = std::clamp(y1, 0, height);
    if (x1 <= x0 || y1 <= y0) return;

    bool anyVisible = false;
    for (const auto& layer : layers) {
        if (layer.visible && layer.rgba) {
            anyVisible = true;
            break;
        }
    }

    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            const size_t i = static_cast<size_t>(y) * static_cast<size_t>(width)
                             + static_cast<size_t>(x);
            if (!anyVisible) {
                inOut[i * 4u + 0] = 255;
                inOut[i * 4u + 1] = 255;
                inOut[i * 4u + 2] = 255;
                inOut[i * 4u + 3] = 255;
                continue;
            }
            const Rgba acc = compositePixelAt(i, layers);
            rgbaToBytes(acc, inOut[i * 4u + 0], inOut[i * 4u + 1], inOut[i * 4u + 2],
                        inOut[i * 4u + 3]);
        }
    }
}

} // namespace PaintLayerBlend
