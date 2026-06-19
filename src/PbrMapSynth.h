#pragma once

#include <QImage>
#include <QString>
#include <vector>

/// #404: PBR map synthesis from a single albedo/diffuse texture.
///
/// Pure-data, NO Ogre and NO Qt-singleton dependency — same shape as
/// NormalMapGenerator / TextureChannelPacker so it unit-tests without a GL
/// context. The ONNX inference path is compiled only when ENABLE_ONNX is set;
/// everything else (NCHW tensor packing, tiling, the normal/height
/// post-processing, and the roughness heuristic) is always available so the
/// non-ONNX pieces can be tested on any build.
///
/// A DeepBump-style UNet predicts a tangent-space NORMAL map and/or a HEIGHT
/// map from albedo. ROUGHNESS is not predicted by such models; it is derived
/// here from albedo luminance via a low-frequency heuristic (the same approach
/// most one-click PBR tools use). When the model emits only height, the NORMAL
/// map is derived from it via the existing Sobel path (NormalMapGenerator).
namespace PbrMapSynth {

struct Options {
    bool generateNormal    = true;
    bool generateRoughness = true;
    bool generateHeight    = true;

    // Model input tile size (HxW). DeepBump-style UNets are trained on small
    // fixed patches, so large textures are processed in overlapping tiles and
    // re-composited. 0 → run the whole image in one shot (only if it fits).
    int tileSize = 256;
    int overlap  = 16;     // tile overlap in px, feathered to hide seams

    float normalStrength = 1.0f;   // XY gradient scale on the decoded normal
    bool  invertG        = false;  // OpenGL (+Y up, default) ↔ DirectX (+Y down)

    bool overwriteCache  = false;  // re-run even if the output PNGs already exist

    // Roughness heuristic knobs: rough = clamp(base + contrast*(1-luma), 0..1),
    // then mildly blurred so it stays low-frequency.
    float roughnessBase     = 0.5f;
    float roughnessContrast = 0.5f;
};

struct Result {
    bool ok = false;
    QString error;
    QImage  normal;     // RGB8 tangent-space — empty if not generated
    QImage  roughness;  // grayscale (Grayscale8) — empty if not generated
    QImage  height;     // grayscale (Grayscale8) — empty if not generated
};

// ── building blocks (always compiled, unit-tested directly) ──────────────────

/// Pack an RGB image into a planar NCHW float buffer normalized to [0,1].
/// `channels` is 1 (grayscale/luminance) or 3 (RGB). Buffer layout is
/// [c0 row-major HxW][c1 ...]..., size = channels*H*W.
std::vector<float> toNCHW(const QImage& rgb, int channels);

/// Inverse of toNCHW for a 3-channel [0,1] tensor → RGB8 image.
QImage nchwToRgb(const std::vector<float>& data, int width, int height);

/// Decode a model normal tensor (3ch, values in [0,1] encoding [-1,1]) into an
/// RGB8 tangent-space normal map, applying strength + invertG.
QImage decodeNormal(const std::vector<float>& data, int width, int height,
                    float strength, bool invertG);

/// Scale a 1-channel [0,1] tensor to a Grayscale8 height image.
QImage decodeHeight(const std::vector<float>& data, int width, int height);

/// Derive a low-frequency roughness map from albedo luminance (no model).
QImage roughnessFromAlbedo(const QImage& albedo, float base, float contrast);

/// Take one channel of a 3-channel planar [0,1] tensor (default luminance) and
/// scale to a Grayscale8 image — for the PBRify roughness/height models, which
/// emit RGB even for single-channel maps.
QImage decodeGrayscaleFromRgb(const std::vector<float>& data, int width, int height);

// ── ONNX inference (only with ENABLE_ONNX) ───────────────────────────────────

/// Run one 3-channel-in/3-channel-out SPAN model at `modelPath` against
/// `albedo`, tiling per `opts` with a feathered seam blend, and return the
/// full-resolution PLANAR RGB float result (3*W*H, channel-planar). On failure
/// returns an empty vector and sets `*error`. `outW`/`outH` get the dimensions.
/// The caller decodes the result per map type (decodeNormal / decodeGrayscaleFromRgb).
/// Without ENABLE_ONNX returns empty + a "not built with ONNX" error.
std::vector<float> runTiledModel(const QImage& albedo, const QString& modelPath,
                                 const Options& opts, int* outW, int* outH,
                                 QString* error);

/// Legacy single-model entry (kept for the original tests): runs `modelPath`
/// and fills normal/height from its output, roughness from the heuristic.
Result synthesize(const QImage& albedo, const QString& modelPath,
                  const Options& opts = {});

} // namespace PbrMapSynth
