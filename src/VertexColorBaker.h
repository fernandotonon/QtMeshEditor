#ifndef VERTEXCOLORBAKER_H
#define VERTEXCOLORBAKER_H

#include "EditableMesh.h"
#include "TexturePaintBuffer.h"

#include <cstddef>
#include <cstdint>
#include <vector>

/**
 * @brief Bake EditableMesh vertex colors into a TexturePaintBuffer.
 *
 * For each triangle, the bake walks UV-space pixels covered by the
 * triangle, computes per-pixel barycentric coordinates from the UV
 * verts, and writes the barycentric-interpolated vertex color into the
 * buffer. The seam-dilation pass then "smears" the rasterized colors
 * outward by N pixels to mask UV-island bleed at MIP-map time.
 *
 * Pure data — no Ogre runtime state beyond the math types on
 * EditableMesh / TexturePaintBuffer.
 */
class VertexColorBaker
{
public:
    struct Options {
        /// Output texture size (square). Buffer will be resized to this.
        int resolution = 1024;
        /// Pixels of edge-dilation applied after rasterization (0 = none).
        /// Each iteration extends rasterized pixels outward by 1 px using
        /// 8-neighbor majority sampling.
        int dilationPixels = 4;
        /// Background color for unrasterized pixels (alpha 0 means
        /// transparent — but PNG savers will store this exactly, so
        /// downstream consumers see a clear seam-mask).
        Ogre::ColourValue background = Ogre::ColourValue(1.0f, 1.0f, 1.0f, 0.0f);
    };

    /**
     * @brief Bake `mesh` into `outBuffer`.
     *
     * If `mesh` has no vertex colors and no submeshes are visited, the
     * buffer is still resized and cleared to the background color.
     *
     * @return number of pixels written by rasterization (before dilation).
     */
    static int bake(const EditableMesh& mesh,
                    TexturePaintBuffer& outBuffer,
                    const Options& options);

    /// Convenience: bake with default options.
    static int bake(const EditableMesh& mesh, TexturePaintBuffer& outBuffer);

    /// Standalone rasterizer for a single triangle in UV space.
    /// `uv0..uv2` are in [0..1]^2; `c0..c2` are colors at the verts.
    /// Updates `outBuffer` and returns the number of pixels written.
    static int rasterizeTriangle(TexturePaintBuffer& outBuffer,
                                 const Ogre::Vector2& uv0,
                                 const Ogre::Vector2& uv1,
                                 const Ogre::Vector2& uv2,
                                 const Ogre::ColourValue& c0,
                                 const Ogre::ColourValue& c1,
                                 const Ogre::ColourValue& c2);

    /**
     * @brief Dilate rasterized pixels outward by `iterations` pixels.
     *
     * Reads `coverage` (true = pixel was filled by rasterization, false
     * = background), then for every false pixel adjacent to a true
     * pixel, copies the first non-background neighbor's color and flips
     * coverage. Repeats `iterations` times.
     *
     * @return number of pixels flipped from background to filled across
     *         all iterations.
     */
    static int dilate(TexturePaintBuffer& buffer,
                      std::vector<uint8_t>& coverage,
                      int iterations);
};

#endif // VERTEXCOLORBAKER_H
