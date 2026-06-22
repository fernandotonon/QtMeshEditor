#pragma once

#include <QImage>
#include <QString>
#include <functional>

/// #405: Real-ESRGAN texture super-resolution via ONNX Runtime.
///
/// Pure-data, no Ogre and no Qt-singleton dependency (same shape as
/// PbrMapSynth / NormalMapGenerator) so it unit-tests without a GL context. The
/// ONNX inference is compiled only when ENABLE_ONNX is set; the tiling/seam
/// helpers are always available so they can be tested on any build.
///
/// The model is a scale-changing ESRGAN/RRDBNet net (3-channel in → 3-channel
/// out, output H*scale × W*scale, values in [0,1]). Large inputs are processed
/// in overlapping tiles whose upscaled results are composited with a feathered
/// blend (in OUTPUT space) to hide seams. The scale factor is discovered from
/// the model's output/input ratio at runtime, not hardcoded.
namespace TextureUpscaler {

struct Options {
    int   tileSize = 256;   // model input tile (px); 0 = whole image in one shot
    int   overlap  = 16;    // input-space tile overlap, feathered to hide seams
    bool  overwriteCache = false;
};

struct Result {
    bool ok = false;
    QString error;
    QImage image;           // upscaled RGB8 — empty on failure
    int scale = 0;          // detected scale factor (e.g. 2 or 4)
};

/// Progress callback: invoked once per tile with (tilesDone, tilesTotal).
/// Return false to CANCEL — upscale() then returns ok=false, error="cancelled".
/// Called on the calling thread (the worker), so the consumer must marshal any
/// UI update back to the GUI thread itself.
using ProgressFn = std::function<bool(int done, int total)>;

/// Upscale `src` by running the ONNX model at `modelPath`, tiling per `opts`.
/// `onProgress` (optional) reports per-tile progress and can cancel. Returns
/// ok=false with a populated `error` when the model can't be loaded/run (the
/// graceful offline/missing-model path) or when cancelled. Without ENABLE_ONNX
/// this always returns ok=false ("not built with ONNX").
Result upscale(const QImage& src, const QString& modelPath, const Options& opts = {},
               const ProgressFn& onProgress = {});

} // namespace TextureUpscaler
