#pragma once

#include <QString>
#include <cstdint>
#include <vector>

#include <cstddef>

/// Minimal OpenEXR 2.0 writer — 3-channel (R,G,B) float32 scanline images,
/// uncompressed. Just enough to round-trip a VAT bake's position+normal
/// texture into a format Unreal Engine's TextureFactory accepts as a
/// non-color HDR source (TC_HDR / TC_VectorDisplacementmap).
///
/// Why we ship our own writer instead of pulling OpenEXR:
///   * Qt has no native EXR support in 6.9.x.
///   * Adding the OpenEXR/Imath dependency for a single one-off code path
///     would balloon the bake module's link surface and CI matrix.
///   * The spec is small enough that a no-compression scanline writer is
///     ~150 LOC and trivially testable.
///
/// Format: spec-compliant subset of OpenEXR 2.0 with these fixed choices:
///   * Magic 0x01312f76, version 2, no tile bit, no long-names bit.
///   * Attributes: channels (R, G, B float32), compression NO_COMPRESSION,
///     dataWindow = displayWindow = (0,0,W-1,H-1), lineOrder INCREASING_Y,
///     pixelAspectRatio 1.0, screenWindowCenter (0,0), screenWindowWidth 1.
///   * Scanline payload: R-row then G-row then B-row per scanline, no
///     compression, no padding.
///
/// Not implemented (intentional):
///   * Tiled / multi-part files.
///   * Compression (zip / piz / pxr24).
///   * Half-precision RGB (only the RGBA variant has a half writer, used
///     by the bone texture's `rgba16f` encoding).
///   * Alpha / Z / sampled-rate / arbitrary attributes.
namespace MinimalEXR {

/// Write a 3-channel float32 scanline EXR to `path`.
///
/// @param path  Output filesystem path. Overwritten if it exists.
/// @param width Image width in pixels (must be > 0).
/// @param height Image height in pixels (must be > 0).
/// @param rgbData Row-major (height × width × 3) float buffer. The pixel at
///                (x, y) is at index (y * width + x) * 3 + {0,1,2} for R/G/B.
///                Caller owns; the writer only reads.
///
/// @return true on success. false if the buffer size mismatches the
///         declared dimensions or the file write fails.
bool writeRGB32F(const QString& path,
                 int width,
                 int height,
                 const std::vector<float>& rgbData);

/// 4-channel variant: `rgbaData` is interleaved RGBA, `width*height*4`
/// floats. Channels are declared A,B,G,R (EXR alphabetical order); a
/// reader asking for R/G/B/A by name gets the caller's layout back.
/// Used by the rigid-body VAT (quaternion x,y,z,w in one texel).
bool writeRGBA32F(const QString& path,
                  int width,
                  int height,
                  const std::vector<float>& rgbaData);

/// Half-float (EXR pixel type HALF) variant of `writeRGBA32F`: the same
/// interleaved RGBA float input, stored as IEEE 754 binary16 (round to
/// nearest even; out-of-range values become +/-Inf). Bone texture
/// `rgba16f` encoding (#1166).
bool writeRGBA16F(const QString& path,
                  int width,
                  int height,
                  const std::vector<float>& rgbaData);

/// IEEE 754 binary32 <-> binary16 conversion (round to nearest even).
/// Public so callers can predict exactly what a half texture will hold.
uint16_t floatToHalf(float value);
float halfToFloat(uint16_t half);

} // namespace MinimalEXR
