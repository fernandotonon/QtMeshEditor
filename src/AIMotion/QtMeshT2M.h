#pragma once

// QtMesh T2M: QtMeshEditor's own text-to-motion model, the small alternative to NVIDIA Kimodo (EXPERIMENTAL).
//
// A flow-matching transformer (22.6 M parameters, scripts/train-t2m-text.py) trained on Kimodo-generated clips,
// CMU MoCap and CC0 game clips, conditioned on a sentence embedding of the prompt. Everything runs natively:
//   text    Qwen3-Embedding-0.6B (Apache-2.0) through the bundled llama.cpp: "Instruct: …\nQuery:<prompt>",
//           last-token pooling, L2-normalised, 1024 floats (the same vectors scripts/embed-t2m-text.py trained on)
//   motion  t2m-text.onnx: inputs text[1,1024], seed[1,Z] ~ N(0,0.5), length[1] frames; output motion[1,T,220]
//           (per canonical role: t, quat x y z w, s), the 24-step Euler sampler with CFG unrolled in the graph
// The output is the 22-role canonical WORLD-frame clip the template library uses, so it rides the same retarget
// (AnimationMerger::applyMotionClip). Rotation only: no root translation.
//
// Clips longer than about two seconds start from a standing pose in the training data (every Kimodo clip begins
// at the start of its generation), so the model holds still for up to a second before moving. generate() asks for
// up to kLeadInFrames more frames and trims the still start (leadInFrames()).
//
// Files (downloaded only on request) under <AppData>/ai_models/qtmesh_t2m/:
//   t2m-text.onnx t2m-text.json Qwen3-Embedding-0.6B-Q8_0.gguf LICENSE-Qwen3-Embedding.txt ATTRIBUTION.md
// (hosted in fernandotonon/QtMeshEditor-models and fernandotonon/QtMeshEditor-t2m, qtmesh_t2m/, SHA-256 pinned)
// Overrides: QTMESH_QTMESH_T2M_BASE_URL / QSettings ai/qtmeshT2mBaseUrl, QTMESH_QTMESH_T2M_NO_DOWNLOAD.

#include <QString>
#include <QStringList>

#include <array>
#include <functional>
#include <vector>

namespace AIMotion {

class QtMeshT2M {
public:
    using Pose = std::vector<std::array<float, 4>>;   ///< one quat (x,y,z,w) per canonical role
    using Frames = std::vector<Pose>;

    struct ModelFile {
        QString name;
        QString url;
        QString sha256;          ///< empty for an overridden base URL
        qint64 approxBytes = 0;
        bool licence = false;
    };

    struct Options {
        QString prompt;
        double duration = 2.0;    ///< seconds; clamped to the model's maximum (kMaxFrames)
        int variations = 1;       ///< 1..16
        bool hasSeed = false;
        int seed = 0;
    };

    struct Clip {
        Frames quats;             ///< [frames][22] canonical world quats
        int trimmedFrames = 0;    ///< still lead-in frames removed
    };

    struct Result {
        bool ok = false;
        QString error;
        std::vector<Clip> clips;  ///< one per variation
        int fps = 30;
        std::vector<std::array<float, 4>> restWorld;
        std::vector<std::array<float, 3>> restDir;
        QString note;             ///< e.g. the duration was clamped
        double encodeSeconds = 0.0;
        double sampleSeconds = 0.0;
    };

    /// stage, done, total; return false to cancel.
    using ProgressFn = std::function<bool(const QString& stage, int done, int total)>;

    static constexpr int kFps = 30;
    static constexpr int kMaxFrames = 150;      ///< the model's T (5 s)
    static constexpr int kLeadInFrames = 30;    ///< extra frames generated for the lead-in trim
    static constexpr float kStillDegPerFrame = 1.5f;

    static QString modelDir();
    static std::vector<ModelFile> modelFiles();
    static bool modelsInstalled(QStringList* missing = nullptr);
    static qint64 missingBytes();
    /// Built with ONNX Runtime + llama.cpp.
    static bool buildSupported();
    /// One line for UIs: ready, or what is missing.
    static QString description();

    /// Fetch what is missing (blocking, MAIN thread: ModelDownloader lives there). Only on an explicit request.
    static bool downloadModels(QString* error, const ProgressFn& progress = {});

    static bool validateOptions(const Options& o, QString* error);
    /// Encode + sample (safe on a worker thread; never downloads).
    static Result generate(const Options& o, const ProgressFn& progress = {});

    /// Leading frames whose mean per-role rotation speed (roles 14 and 18 excluded: the model never learned them)
    /// stays below `stillDegPerFrame`, at most `maxTrim`. Pure.
    static int leadInFrames(const Frames& q, int maxTrim, float stillDegPerFrame = kStillDegPerFrame);
    /// Frames to request and the most that may be trimmed for a wanted length. Pure.
    static void planFrames(int wanted, int* request, int* maxTrim);
};

} // namespace AIMotion
