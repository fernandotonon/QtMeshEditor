#ifndef PAINTLAYERBLEND_H
#define PAINTLAYERBLEND_H

#include <cstdint>
#include <vector>

/**
 * @brief Per-pixel blend modes for the Paint v2 layer stack (#546).
 *
 * Pure data — no Qt or Ogre runtime dependencies. Operates on
 * straight (non-premultiplied) RGBA in [0..1].
 */
namespace PaintLayerBlend {

enum class Mode {
    Normal = 0,
    Multiply,
    Screen,
    Overlay,
    Add,
    Subtract,
    SoftLight,
    Hue,
};

/// Human-readable names for UI / persistence.
const char* modeName(Mode mode);
Mode modeFromName(const char* name);

struct Rgba {
    float r = 0.f;
    float g = 0.f;
    float b = 0.f;
    float a = 1.f;
};

/// Blend `src` over `dst` with `mode`, `opacity`, and optional mask
/// (mask 0..255, 255 = full layer contribution at this pixel).
Rgba blendPixel(const Rgba& dst, const Rgba& src, Mode mode, float opacity, uint8_t mask = 255);

/// Composite `layers` bottom-up into `out` (RGBA8, width*height*4).
/// Each layer entry is RGBA8 + optional maskAlpha (empty = all 255).
struct LayerInput {
    const uint8_t* rgba = nullptr;
    const uint8_t* maskAlpha = nullptr; ///< nullptr → fully visible
    Mode blendMode = Mode::Normal;
    float opacity = 1.f;
    bool visible = true;
};

void compositeLayers(int width, int height,
                     const std::vector<LayerInput>& layers,
                     std::vector<uint8_t>& out);

/// Recomposite only @p x0..x1 × @p y0..y1 into @p inOut (existing RGBA8).
void compositeLayersRegion(int width, int height,
                           const std::vector<LayerInput>& layers,
                           uint8_t* inOut,
                           int x0, int y0, int x1, int y1);

/// Convenience: byte [0..255] → float [0..1].
inline float byteToF(uint8_t v) { return static_cast<float>(v) * (1.f / 255.f); }
inline uint8_t fToByte(float v)
{
    if (v <= 0.f) return 0;
    if (v >= 1.f) return 255;
    return static_cast<uint8_t>(v * 255.f + 0.5f);
}

Rgba rgbaFromBytes(uint8_t r, uint8_t g, uint8_t b, uint8_t a);
void rgbaToBytes(const Rgba& c, uint8_t& r, uint8_t& g, uint8_t& b, uint8_t& a);

} // namespace PaintLayerBlend

#endif // PAINTLAYERBLEND_H
