#pragma once

#include <QImage>
#include <QString>

/// Slice H: tangent-space normal map generator from a grayscale
/// height/bump source via a 3×3 Sobel kernel. Pure-data, no Ogre — same
/// shape as TextureChannelPacker so it can be unit-tested without GL.
///
/// Convention notes:
///  - Output is RGB8 (no alpha) by default. The Z component of the
///    surface normal is always positive in tangent space, so the blue
///    channel sits in [128..255]. This matches the Ogre/glTF convention
///    and what most engines expect.
///  - `invertR` flips the red channel (rare; some pipelines flip it for
///    handedness consistency).
///  - `invertG` is the **DirectX vs OpenGL** difference. Default is
///    OpenGL (+Y up). Set true for DirectX-flavoured normal maps
///    (e.g. Unity / Unreal default sometimes ship DX-flavour assets).
namespace NormalMapGenerator {

struct GenSpec {
    QString sourcePath;        // grayscale height/bump map
    float   strength = 2.0f;   // Sobel gradient multiplier (effective bump intensity)
    int     outputWidth = 0;   // 0 → use source size
    int     outputHeight = 0;
    bool    invertR = false;
    bool    invertG = false;   // ← DirectX (+Y down) when true
};

struct GenResult {
    bool ok = false;
    QString error;
    QImage  image;             // RGB8 — empty on failure
    int     usedWidth = 0;
    int     usedHeight = 0;
};

/// Generate a tangent-space normal map. Returns the image and an `ok`
/// flag; check `error` on failure.
GenResult generate(const GenSpec& spec);

/// Convenience: generate and save to `outPath`. Extension drives format.
GenResult generateToFile(const GenSpec& spec, const QString& outPath);

} // namespace NormalMapGenerator
