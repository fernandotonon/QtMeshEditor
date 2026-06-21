#pragma once

#include <QImage>
#include <QString>

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

/// Upscale `src` by running the ONNX model at `modelPath`, tiling per `opts`.
/// Returns ok=false with a populated `error` when the model can't be
/// loaded/run (the graceful offline/missing-model path). Without ENABLE_ONNX
/// this always returns ok=false ("not built with ONNX").
Result upscale(const QImage& src, const QString& modelPath, const Options& opts = {});

} // namespace TextureUpscaler
