#pragma once
// Pillow-compatible image downscale for the TRELLIS.2 input.
//
// TRELLIS.2's own preprocess_image shrinks the input to a 1024 px long side
// with Pillow's LANCZOS filter before cropping. The 1024 texture flow is very
// sensitive to sub-pixel differences in its input (measured: the same matte
// resampled differently flips a navy suit between metallic 0.0 and 0.8 at a
// fixed seed), so we reproduce Pillow's resampler bit for bit instead of using
// QImage::scaled or stb: separable Lanczos-3, horizontal pass first, 22-bit
// fixed-point coefficients, and for RGBA the premultiply / unpremultiply
// round trip Pillow does around the resample.

#include <QImage>
#include <QSize>

#include <cstdint>
#include <vector>

namespace PillowResample {

// Target size of upstream's `scale = min(1, 1024 / max(w, h))` +
// `(int(w * scale), int(h * scale))`. Returns `size` unchanged when it already
// fits.
QSize fitLongSide(QSize size, int maxSide = 1024);

// Lanczos resample of an 8-bit, 4-channel buffer (rows packed). Channels are
// filtered independently, as Pillow does for "RGBa"; pass premultiplied colour
// for RGBA images. Pure data, no Qt.
std::vector<uint8_t> lanczosRgba8(const std::vector<uint8_t>& rgba, int w, int h,
                                  int outW, int outH);

// Pillow's RGBA -> RGBa (premultiply) and RGBa -> RGBA (unpremultiply).
void premultiply(std::vector<uint8_t>& rgba);
void unpremultiply(std::vector<uint8_t>& rgba);

// Image.resize(size, LANCZOS) on an RGBA image, as Pillow does it
// (premultiply, resample, unpremultiply). Returns Format_RGBA8888.
QImage resizeLikePillow(const QImage& image, QSize size);

} // namespace PillowResample
