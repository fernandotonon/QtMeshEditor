#pragma once

#include <QImage>
#include <QString>

/// #1018 (epic #818 Track C4): monocular depth estimation from a PHOTO via
/// Depth-Anything-V2-**Small**.
///
/// Pure-data — no Ogre, no Qt singleton (the PbrMapSynth / TextureUpscaler
/// shape), so the geometry helpers unit-test without a GL context. ONNX
/// inference is compiled only under ENABLE_ONNX; the pre/post-processing is
/// always available and always tested.
///
/// NOT to be confused with `MeshDepthRenderer`, which RENDERS a depth map of a
/// mesh already in the scene. This ESTIMATES depth from a 2-D photograph.
/// Both deliberately emit the same convention — 8-bit grayscale, **near =
/// bright** (MiDaS/ControlNet) — so either can feed
/// `SDWorker::generateTextureControlled` interchangeably.
///
/// LICENCE: only the **Small** variant may ever ship. Small is Apache-2.0;
/// Base and Large are CC-BY-NC-4.0 and fail the project's permissive-
/// redistribution bar. `scripts/export-depth-anything-onnx.py` refuses to
/// export anything else. See THIRD_PARTY_AI_MODELS.md.
///
/// MODEL CONTRACT (fixed by the export, asserted there against torch):
///   input   pixel_values     float32 [1,3,518,518]  ImageNet-normalised RGB
///   output  predicted_depth  float32 [1,518,518]    relative inverse depth,
///                                                   larger = NEARER
/// The input size is **pinned**, not dynamic: a dynamic-axis export traces
/// clean but drifts from the traced resolution (measured 2.3e-02 relative error
/// at 462² vs 2.7e-06 at 518²) because the ViT position-embedding interpolation
/// is only partly captured. The reference preprocessor resizes everything to
/// 518 anyway, so pinning costs nothing and removes a silent-wrong-answer mode.
namespace PhotoDepth {

/// The model's pinned input edge length (also the output edge length).
inline constexpr int kInputSize = 518;

struct Options {
    /// Output edge length. 0 = the SOURCE image's own size (the common case:
    /// a control image should match the image it conditions). The model always
    /// runs at kInputSize internally; this only controls the final resize.
    int outputSize = 0;
    /// Preserve the source aspect ratio by letterboxing into the square model
    /// input instead of stretching. Off by default because ControlNet depth is
    /// used as a whole-frame conditioning image, and the reference pipeline
    /// stretches; enable when the aspect distortion matters more than coverage.
    bool letterbox = false;
};

struct Result {
    bool ok = false;
    QString error;
    /// Grayscale8, near = bright. Empty on failure.
    QImage depth;
    /// Raw model output range before normalisation — useful for diagnostics
    /// (a constant map means the model saw nothing it understood).
    float rawMin = 0.0f;
    float rawMax = 0.0f;
};

/// Absolute path the model is cached at (AppData/ai_models/depth/da2_small.onnx).
QString modelPath();
bool modelPresent();
/// Download on first use (blocks via a local ModelDownloader event loop).
/// Returns the path, or empty when offline/disabled/failed. Honours
/// QTMESH_DEPTH_NO_DOWNLOAD and the base-URL override
/// QTMESH_DEPTH_MODEL_BASE_URL / QSettings ai/depthModelBaseUrl.
QString ensureModelBlocking();

/// True only when built with ENABLE_ONNX.
bool isAvailable();

/// Estimate depth for `photo` using the ONNX model at `modelPath`.
/// On any failure (no ONNX, missing model, inference error) returns ok=false
/// with a reason and an empty image — never a partially-valid map.
Result estimate(const QImage& photo, const QString& modelPath,
                const Options& opts = {});

// ---- Pure helpers (always compiled; unit-tested without ONNX) -------------

/// ImageNet-normalise `photo` into the model's NCHW input buffer, resizing to
/// kInputSize. Returns kInputSize*kInputSize*3 floats in CHW order.
/// `letterbox` pads with black to preserve aspect instead of stretching.
std::vector<float> toModelInput(const QImage& photo, bool letterbox);

/// Normalise raw model output to Grayscale8 with **near = bright**, and report
/// the pre-normalisation range. A constant field maps to mid-grey rather than
/// dividing by zero.
QImage toDepthImage(const std::vector<float>& raw, int w, int h,
                    float* outMin = nullptr, float* outMax = nullptr);

/// Undo a letterbox: crop the padded region back out of a square depth map for
/// a source of the given aspect. Returns `square` unchanged when it was not
/// letterboxed (srcW/srcH already square) or on degenerate input.
QImage cropLetterbox(const QImage& square, int srcW, int srcH);

} // namespace PhotoDepth
