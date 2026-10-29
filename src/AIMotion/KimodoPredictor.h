#pragma once

// NVIDIA Kimodo text-to-motion backend. Two runtimes produce the same motion interchange files:
//   native  KimodoNative: llama.cpp text encoder + ONNX Runtime motion model, no Python (the default once its
//           models are downloaded; matches Kimodo's PyTorch output on the same noise)
//   python  the out-of-process sidecar (ai/kimodo/generate.py); also the only runtime for constraints files and
//           the mock
// Options::runtime picks one ("" / "auto" = native when ready, else python when installed).
//
// Python does inference only. Everything after the NPZ (SOMA -> rig mapping,
// retarget, keyframes, export) is QtMeshEditor C++ (SomaRetarget). Python is
// never needed to START the editor: the runtime is probed only when a Kimodo
// generation is requested.
//
// Runtime resolution (same shape as the TRELLIS.2 sidecar):
//   dir     QTMESH_KIMODO_ENV       | QSettings ai/kimodoEnv    | <AppData>/kimodo
//   python  QTMESH_KIMODO_PYTHON    | QSettings ai/kimodoPython | runtime.json "python" | <dir>/env/bin/python
//   script  QTMESH_KIMODO_GENERATE  | runtime.json "generate"   | <dir>/generate.py
//   mock    QTMESH_KIMODO_MOCK=1 (or Options::mock): synthetic motion, numpy only
//
// Credentials: the gated Llama text encoder reads the user's own Hugging Face
// login (`hf auth login` / HF_TOKEN in the environment). Nothing here reads,
// stores, passes on the command line or logs a token.

#include <QJsonObject>
#include <QString>
#include <QStringList>

#include <functional>
#include <vector>

namespace AIMotion {

struct MotionSegment {
    QString text;
    double duration = 5.0;   ///< seconds
};

class KimodoPredictor {
public:
    struct Options {
        std::vector<MotionSegment> segments;   ///< one per timeline segment; >= 1
        int numSamples = 1;                    ///< variations, 1..16
        bool hasSeed = false;
        int seed = 0;
        int diffusionSteps = 0;                ///< 0 = the model's default
        bool rootMotion = true;
        bool postprocess = true;               ///< Kimodo's foot-skate cleanup
        QString constraintsPath;               ///< Kimodo constraints JSON (optional)
        QString model = QStringLiteral("Kimodo-SOMA-RP-v1.2");
        QString textEncoderUrl;                ///< reuse a running kimodo_textencoder service
        /// Llama text encoder weights: "" (= the sidecar default, auto) | auto | none (bf16) | int8 | int4.
        /// auto = int8, quantized once locally from the user's own Llama download (~8.7 GB GPU memory instead of
        /// ~15 GB, embedding cosine 0.9998 against bf16).
        QString textEncoderQuant;
        QString outputDir;                     ///< empty = a fresh temp dir under AppData
        int timeoutSec = 3600;
        bool mock = false;
        /// "" or "auto" (native when its models are downloaded, else the Python sidecar) | "native" | "python".
        QString runtime;
    };

    struct Result {
        bool ok = false;
        QString error;
        QStringList files;          ///< motion_NN.npz, one per variation
        QJsonObject manifest;       ///< the sidecar's result.json / done event
        QString outputDir;
    };

    /// stage = import|load_model|generate|write; return false to cancel.
    using ProgressFn = std::function<bool(const QString& stage, int done, int total)>;

    static QString runtimeDir();
    static QString pythonPath();
    static QString generateScriptPath();
    static bool mockRequested(const Options& o);
    /// Accepted Options::textEncoderQuant values (besides empty).
    static QStringList textEncoderQuantModes() { return {QStringLiteral("auto"), QStringLiteral("none"), QStringLiteral("int8"), QStringLiteral("int4")}; }
    /// Some runtime can generate (native ready, or python + script present).
    static bool runtimeAvailable();
    /// One line for UIs: which runtime will run, or how to get one.
    static QString runtimeDescription();
    /// The Python sidecar alone.
    static bool pythonRuntimeAvailable();
    static QString pythonRuntimeDescription();
    /// Accepted Options::runtime values.
    static QStringList runtimeModes() { return {QStringLiteral("auto"), QStringLiteral("native"), QStringLiteral("python")}; }
    /// Upper bound of Options::diffusionSteps: Kimodo's training schedule has 1000 steps.
    static constexpr int kMaxDiffusionSteps = 1000;
    /// Range/enum checks shared by the CLI, MCP and predict() (run them BEFORE any model download).
    static bool validateOptions(const Options& o, QString* error);
    /// "native" | "python" for these options, or "" with `why` set when nothing can run them.
    static QString resolveRuntime(const Options& o, QString* why = nullptr);

    /// Command-line arguments for the sidecar (exposed for tests).
    static QStringList buildArguments(const Options& o, const QString& outputDir);

    static Result predict(const Options& o, const ProgressFn& progress = {});
};

} // namespace AIMotion
