#pragma once

#include <QImage>
#include <QString>
#include <array>

/// Slice G: pack 1-4 grayscale source images into the RGBA channels of a
/// single output texture. Common indie game-dev pattern — Unity ORM
/// (Occlusion R / Roughness G / Metallic B), Unreal MR (Metallic R /
/// Roughness G / unused B), Lumberyard, etc.
///
/// Each output channel takes either a source image (sampled as
/// luminance) or a constant value. Output size defaults to the largest
/// source dimensions; smaller sources are bilinear-scaled to match.
namespace TextureChannelPacker {

/// One output channel's source. Either a path to a PNG/TGA/JPG/BMP
/// (sampled as Rec.601 luminance) or a constant 0..1 value when path
/// is empty.
struct ChannelSource {
    QString path;          // empty → use constantValue
    float constantValue = 0.0f;
    bool invert = false;   // useful for converting roughness ↔ glossiness
};

struct PackingSpec {
    ChannelSource red;
    ChannelSource green;
    ChannelSource blue;
    ChannelSource alpha;
    int outputWidth  = 0;   // 0 → max of source widths (defaults to 256 if all constant)
    int outputHeight = 0;
    bool includeAlpha = true;
};

struct PackResult {
    bool ok = false;
    QString error;
    QImage  image;          // RGBA8 (or RGB888 if !includeAlpha) — empty on failure
    int     usedWidth = 0;
    int     usedHeight = 0;
};

/// Pack the four channels into a single QImage. Pure-data; safe to call
/// without Ogre. Empty paths use constantValue. Bilinear scales smaller
/// sources up to the output dimensions.
PackResult pack(const PackingSpec& spec);

/// Convenience: pack and write to a PNG/TGA/JPG file at `outPath`. The
/// extension determines the format. Returns the same PackResult; check
/// `ok` for success and `error` for the message on failure.
PackResult packToFile(const PackingSpec& spec, const QString& outPath);

} // namespace TextureChannelPacker
