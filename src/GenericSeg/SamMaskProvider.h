#pragma once
// SAM 2.1 mask provider for generic segmentation (#1179): runs the two graphs
// exported by scripts/export-sam2-onnx.py on each rendered view (encoder once,
// decoder on a foreground point grid) and returns filtered, de-duplicated
// masks. Needs ENABLE_ONNX; otherwise ok() is false with a message.

#include "GenericSegmenter.h"
#include "SamMasks.h"

#include <QString>

#include <functional>
#include <memory>

namespace GenericSeg {

class SamMaskProvider : public MaskProvider {
public:
    struct Options {
        int pointsPerSide = 16;   // 32 found the same parts on test meshes at 2.5x the time
        int batch = 64;            // prompts per decoder run
        Sam::FilterOptions filter;
        bool preferGpu = false;    // CoreML/CUDA when available
        std::function<bool()> cancelled;   // polled between decoder batches
    };

    struct Timing {
        double encodeMs = 0, decodeMs = 0, postMs = 0;
        int images = 0, prompts = 0, candidates = 0, kept = 0;
    };

    SamMaskProvider(const QString& encoderPath, const QString& decoderPath, const Options& opts);
    SamMaskProvider(const QString& encoderPath, const QString& decoderPath);
    ~SamMaskProvider() override;

    bool ok() const;
    QString error() const;
    std::vector<Mask> masksFor(const ViewImage& view) override;
    Timing timing() const;

    // Model files: <dir>/sam2.1_hiera_small_{encoder,decoder}.onnx.
    static QString encoderFileName();
    static QString decoderFileName();

    // Where the app keeps them: <AppData>/ai_models/segment/ (next to the
    // other segmentation models), with Meta's Apache-2.0 text beside them.
    static QString modelDir();
    static bool modelsPresent();
    // First use: downloads the graphs + licence (~160 MB) from the
    // QtMeshEditor-models HF repo, pinned to a commit and checked against
    // SHA-256 (also an existing file, so a corrupt copy is replaced).
    // Overrides: QSettings ai/segmentModelBaseUrl, QTMESH_SEGMENT_MODEL_BASE_URL
    // (a mirror: no digest check), QTMESH_SEGMENT_NO_DOWNLOAD (verify only).
    // Main thread only (ModelDownloader). `cancelled` is polled during the
    // transfer. Returns false with the reason in `error`.
    static bool ensureModelsBlocking(QString* error = nullptr,
                                     const std::function<bool()>& cancelled = {});

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

} // namespace GenericSeg
