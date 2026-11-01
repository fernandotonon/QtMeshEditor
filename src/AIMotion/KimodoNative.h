#pragma once

// Native (Python-free) NVIDIA Kimodo runtime: the text encoder through llama.cpp (KimodoTextEncoder) and the
// motion model through ONNX Runtime, with the sampling loop, classifier-free guidance, segment stitching and
// MotionCorrection post-processing in C++ — a transcription of scripts/verify_kimodo_native.py, which matches
// Kimodo's own PyTorch generation on the same text features and noise (max rotation error 1e-3, root 0.003 cm,
// identical foot contacts; single prompt, two segments, with and without post-processing).
//
// Files (downloaded only on request, SHA-256 pinned for the default hosting), under <AppData>/ai_models/kimodo/:
//   kimodo_denoiser.onnx kimodo_decode.onnx kimodo_encode.onnx kimodo_transition.onnx kimodo_meta.json
//     + LICENSE-NVIDIA-OML.txt NOTICE-Kimodo.txt          (NVIDIA Open Model License: Kimodo-SOMA-RP-v1.2)
//   Llama-3-LLM2Vec-8B-Kimodo-TextEncoder-Q8_0.gguf
//     + LICENSE-Llama-3.txt USE_POLICY-Llama-3.md NOTICE-Llama-3.txt LICENSE-LLM2Vec-MIT.txt
//       (Meta Llama 3 Community License; the merged LLM2Vec adapters are MIT)
// Overrides: QTMESH_KIMODO_MODEL_BASE_URL / QSettings ai/kimodoModelBaseUrl (the ONNX set),
// QTMESH_KIMODO_TEXTENC_BASE_URL / ai/kimodoTextEncoderBaseUrl (the GGUF), QTMESH_KIMODO_NO_DOWNLOAD.

#include "KimodoPredictor.h"

#include <QString>
#include <QStringList>

namespace AIMotion {

class KimodoNative {
public:
    struct ModelFile {
        QString name;       ///< file name under modelDir()
        QString url;        ///< resolved download URL
        QString sha256;     ///< empty for an overridden base URL (another export may differ)
        qint64 approxBytes = 0;
        bool licence = false;   ///< a licence/notice text (no digest: small, not executable)
    };

    static QString modelDir();
    static std::vector<ModelFile> modelFiles();
    /// Every required file present (licence texts included). `missing` gets the names that are not.
    static bool modelsInstalled(QStringList* missing = nullptr);
    /// Approximate download size of what is missing, bytes.
    static qint64 missingBytes();
    /// Built with ONNX Runtime + llama.cpp.
    static bool buildSupported();
    /// One line for UIs.
    static QString description();

    /// Fetch whatever is missing (blocking; call on the MAIN thread — ModelDownloader lives there).
    /// The caller must only do this on an explicit user request (CLI --download, MCP download:true, GUI button).
    static bool downloadModels(QString* error, const KimodoPredictor::ProgressFn& progress = {});

    /// Same Options/Result contract as the Python sidecar: writes motion_NN.npz + result.json to outputDir.
    /// Safe on a worker thread (no downloads here).
    static KimodoPredictor::Result predict(const KimodoPredictor::Options& o, const KimodoPredictor::ProgressFn& progress = {});
};

} // namespace AIMotion
