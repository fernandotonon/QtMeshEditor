#ifndef TEXTURE_INPAINT_H
#define TEXTURE_INPAINT_H

#include <QImage>
#include <QString>

#include <cstdint>
#include <vector>

// #1017 (epic #818, Track C3): texture inpainting with LaMa.
//
// Fills masked regions of a texture with plausible, seamlessly-continued
// content. Two shipped consumers, both of which previously had to live with
// visible artifacts:
//
//   1. UV-island seam/bleed fill after a bake (MeshGenBaker /
//      MultiViewTextureBaker). A bake only writes texels a chart actually
//      covers; everything else keeps the clear colour. The bakers dilate chart
//      borders a few texels so bilinear filtering and MIPs do not drag that
//      clear colour into the seams, but dilation is a nearest-neighbour smear:
//      it stops the bleed, it does not CONTINUE the texture. Inpainting the
//      uncovered texels does.
//   2. A user-facing "Inpaint" brush in texture-paint mode: paint over a
//      blemish, and the region is refilled from its surroundings.
//
// Model: **LaMa** (Suvorov et al., "Resolution-robust Large Mask Inpainting
// with Fourier Convolutions", WACV 2022) — Apache-2.0 code AND weights, so it
// clears the project's permissive-redistribution bar (the same test TripoSR /
// UniRig / BiRefNet passed). See THIRD_PARTY_AI_MODELS.md.
//
// Contract of the graph we host (measured, not assumed — see
// scripts/export-lama-onnx.py):
//     input   image   float32 [1,3,512,512]  RGB scaled to [0,1]
//     input   mask    float32 [1,1,512,512]  1 = inpaint this texel, 0 = keep
//     output  output  float32 [1,3,512,512]  RGB already in 0..255
// Note the ASYMMETRY: the image is fed in [0,1] but the result comes back in
// 0..255. Feeding an un-scaled image saturates the output to near-white
// (measured mean 255.0 vs a correct 156.0), which is silent — it produces a
// plausible-looking image, not an error. kInputSize is PINNED for the same
// reason as the depth and matting exports.
//
// Ogre-free + Qt-only (QImage in/out), the PbrMapSynth / BackgroundRemover
// shape, so the pure pieces unit-test without a GL context. ENABLE_ONNX-guarded;
// without it isAvailable() is false and inpaint() reports why rather than
// silently returning the input (a silent pass-through would look like "the
// inpaint did nothing").
namespace TextureInpaint {

/// The model's pinned spatial size. Tiles larger inputs; see Options::tileSize.
inline constexpr int kInputSize = 512;

struct Options {
    /// Tile size fed to the model. Must be kInputSize — the graph's spatial
    /// dims are pinned — but kept as a field so the tiling maths is explicit
    /// and testable.
    int tileSize = kInputSize;
    /// Overlap between tiles, feathered to hide seams (PbrMapSynth's approach).
    /// Inpainting is context-driven, so neighbouring tiles must see shared
    /// surroundings or the fill changes character at a tile boundary.
    int overlap = 64;
    /// Grow the mask by this many texels before inpainting. A mask that ends
    /// exactly at the bad pixels leaves the model conditioned on the half-bad
    /// texels just outside it, which drags the artifact back in. Bakers'
    /// coverage masks are especially prone to this because the dilation pass
    /// has already smeared clear-colour into the ring around each chart.
    int maskDilatePx = 2;
    /// Composite the model output back only where the mask is set, keeping
    /// every unmasked texel bit-exact. The model does preserve them (measured
    /// MAE 0.0000 outside the mask), but relying on that would make output
    /// quality silently dependent on the graph; compositing makes it structural.
    bool compositeMaskedOnly = true;
};

struct Result {
    bool ok = false;
    QString error;
    QImage image;          ///< the inpainted texture (RGB888)
    bool usedModel = false;
    int maskedTexels = 0;  ///< how many texels were actually filled
};

// ── model management (mirrors PhotoDepth / BackgroundRemover) ───────────────

/// True when this build can run the model at all (ENABLE_ONNX).
bool isAvailable();

/// Absolute path the model is cached at, whether or not it exists yet.
QString modelPath();

/// True when the model file is already on disk.
bool modelPresent();

/// Download the model if absent and return its path (empty on failure).
/// Blocking, main-thread only (nested QEventLoop), like every other predictor.
QString ensureModelBlocking();

// ── pure helpers (always compiled, unit-tested directly) ────────────────────

/// Pack an RGB tile into the planar NCHW [0,1] buffer the graph expects.
/// Size = 3*tile*tile. A null/short image yields a zero buffer of the right
/// size rather than an empty one, so a caller cannot feed a mis-shaped tensor.
std::vector<float> toImageTensor(const QImage& rgbTile, int tile);

/// Pack a mask tile into the planar NCHW buffer: 1.0 where the mask says
/// "inpaint", 0.0 elsewhere. Any non-zero mask pixel counts as set, so callers
/// may pass either a 0/255 bitmap or a soft mask.
std::vector<float> toMaskTensor(const QImage& maskTile, int tile);

/// Decode the model's 0..255 planar RGB output back to an image.
/// Values are clamped, and non-finite values fall back to the SOURCE pixel so a
/// numerically broken graph degrades to "unchanged" rather than to noise.
QImage fromOutputTensor(const std::vector<float>& out, int w, int h,
                        const QImage& source);

/// Grow a binary mask by `px` texels (8-connected), the maskDilatePx step.
QImage dilateMask(const QImage& mask, int px);

/// Count texels the mask marks for inpainting.
int countMasked(const QImage& mask);

/// Build a mask from a baker's per-texel coverage bitmap: texels NOT covered
/// by any chart are the ones to fill. `covered` is row-major, w*h entries.
/// This is the bridge for consumer 1 — the bakers already compute exactly this.
QImage maskFromCoverage(const std::vector<uint8_t>& covered, int w, int h);

// ── inference ──────────────────────────────────────────────────────────────

/// Inpaint `texture` wherever `mask` is set, using the model at `modelFile`.
/// `mask` is resized to the texture if it does not already match. Returns
/// ok=false with a reason on a non-ONNX build, a missing model, an empty mask,
/// or an inference failure — never a silent pass-through.
Result inpaint(const QImage& texture, const QImage& mask,
               const QString& modelFile, const Options& opts = {});

} // namespace TextureInpaint

#endif // TEXTURE_INPAINT_H
