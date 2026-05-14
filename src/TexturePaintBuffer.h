#ifndef TEXTUREPAINTBUFFER_H
#define TEXTUREPAINTBUFFER_H

#include <OgreColourValue.h>
#include <OgreVector.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

/**
 * @brief RGBA8 pixel buffer with dirty-rect tracking for texture painting.
 *
 * Owns a flat `std::vector<uint8_t>` of size `width * height * 4` (RGBA8,
 * top-left origin, UV (0,0) maps to pixel (0, height-1) — i.e. V is flipped).
 *
 * All mutations expand `dirtyRect()`. The dirty rect is the smallest pixel
 * AABB covering every pixel mutated since the last `clearDirty()` call. It
 * is empty when no mutation has happened or after `clearDirty()`.
 *
 * Pure data. Has no Qt or Ogre runtime dependencies — the only Ogre types
 * referenced (`ColourValue`, `Vector2`) are header-only math.
 */
class TexturePaintBuffer
{
public:
    /// Dirty rect in pixel coordinates. [x0..x1) × [y0..y1) — half-open.
    struct DirtyRect {
        int x0 = 0;
        int y0 = 0;
        int x1 = 0;
        int y1 = 0;
        bool empty() const { return x1 <= x0 || y1 <= y0; }
        int width() const { return empty() ? 0 : x1 - x0; }
        int height() const { return empty() ? 0 : y1 - y0; }
    };

    TexturePaintBuffer() = default;

    /// Construct with given size, all pixels initialized to opaque white.
    TexturePaintBuffer(int width, int height);

    /// Resize the buffer, clearing it to opaque white. Marks no dirty rect.
    void resize(int width, int height);

    /// Fill the buffer with a single color. Marks the full buffer dirty.
    void clear(const Ogre::ColourValue& color);

    int width() const { return m_width; }
    int height() const { return m_height; }
    /// Raw RGBA8 byte buffer (row-major, top-left origin).
    const std::vector<uint8_t>& data() const { return m_pixels; }
    std::vector<uint8_t>& data() { return m_pixels; }
    /// Current dirty rect (in pixel coords). Empty when no mutation pending.
    const DirtyRect& dirtyRect() const { return m_dirty; }
    void clearDirty() { m_dirty = {}; }
    /// Expand the dirty rect manually. Used when external code mutates the
    /// raw `data()` array (e.g. VertexColorBaker dilation).
    void markDirty(int x0, int y0, int x1, int y1) { expandDirty(x0, y0, x1, y1); }

    /**
     * @brief Flood-fill connected pixels at (sx, sy) with `fill`.
     *
     * 4-connected scan, tolerance ε=4/255 per channel. Stops at any
     * pixel whose color differs from the seed. Returns the pixel count
     * filled, 0 if the seed already matches `fill`.
     */
    int floodFill(int sx, int sy, const Ogre::ColourValue& fill);

    /// Read pixel. Out-of-bounds returns black-transparent.
    Ogre::ColourValue pixel(int x, int y) const;

    /// Write pixel (clamped to bounds). Expands dirty rect.
    void setPixel(int x, int y, const Ogre::ColourValue& color);

    /**
     * @brief Paint a circular brush stamp at UV coordinate.
     *
     * @param uv         Center UV in [0..1]^2. V is flipped (0 = bottom).
     * @param radiusUV   Brush radius in UV-space units.
     * @param color      Brush color (alpha is interpreted as flow).
     * @param strength   0..1 — how much the brush moves the pixel toward `color`.
     * @param falloff    0 = hard, 1 = full quadratic falloff at the edge.
     * @return Number of pixels modified.
     *
     * Each pixel inside the brush footprint is lerped:
     *
     *     out = lerp(prev, color, strength * weight)
     *
     * where `weight = 1` at center and falls off to `0` at the edge. The
     * falloff curve is `(1 - r^2)^p` with `p = 1 + falloff * 3`, matching
     * the vertex-paint brush in EditModeController.
     */
    int paintBrush(const Ogre::Vector2& uv,
                   float radiusUV,
                   const Ogre::ColourValue& color,
                   float strength = 1.0f,
                   float falloff = 0.5f);

    /**
     * @brief Save the buffer to disk as a PNG/JPEG/TGA/BMP/etc.
     *
     * Uses Qt's QImage internally. Returns false if Qt can't write the
     * given format (typically a typo in the extension).
     *
     * @param path        Output file path. Extension determines format.
     * @return true on success.
     */
    bool save(const std::string& path) const;

    /**
     * @brief Load a PNG/JPEG/TGA/BMP/etc into the buffer.
     *
     * Replaces buffer contents and resizes if needed. The loaded image is
     * converted to RGBA8. Mark loaded contents fully dirty so any consumer
     * (e.g. an Ogre HardwarePixelBuffer mirror) re-uploads.
     *
     * @return true on success.
     */
    bool load(const std::string& path);

    /// Convenience: map a UV to integer pixel coordinates.
    /// V is flipped: uv.y=0 → y=height-1, uv.y=1 → y=0. Caller is responsible
    /// for clamping to bounds.
    void uvToPixel(const Ogre::Vector2& uv, int& outX, int& outY) const;

    /// Convenience: map a pixel back to UV center. Inverse of uvToPixel.
    Ogre::Vector2 pixelToUV(int x, int y) const;

private:
    void expandDirty(int x0, int y0, int x1, int y1);

    int m_width = 0;
    int m_height = 0;
    std::vector<uint8_t> m_pixels;
    DirtyRect m_dirty;
};

#endif // TEXTUREPAINTBUFFER_H
