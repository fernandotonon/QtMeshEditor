#ifndef AUDIOTOFACE_A2FPREDICTOR_H
#define AUDIOTOFACE_A2FPREDICTOR_H

// Audio2Face (#1019): audio in, ARKit blendshape weights out.
//
// Chains the four pure-data pieces around an ONNX session:
//
//   samples -> AudioPreprocess (mono, 16 kHz, centred window)
//           -> network.onnx    (301 coefficients per frame)
//           -> A2FCoefficients (PCA -> ~61.5k vertex deltas)
//           -> BlendshapeSolve (least-squares -> 52 ARKit weights)
//
// The last step is what makes the feature possible at all. The network
// predicts a deformation of NVIDIA's OWN character, which on its own is
// useless against a user's mesh — but NVIDIA also ships that character's
// ARKit-52 blendshape deltas, so the solve runs in their mesh space and
// returns WEIGHTS. Weights carry no topology, so they drive any mesh with
// ARKit targets, including the ones `qtmesh facerig` generates. No
// correspondence between the two meshes is ever needed.
//
// Everything ONNX-facing is `ENABLE_ONNX`-guarded; a build without it reports
// "rebuild with -DENABLE_ONNX" rather than failing obscurely.
//
// Model files (~320 MB total) download on first use from NVIDIA's Hugging
// Face repo. The weights are under the NVIDIA Open Model License: commercial
// use and redistribution are permitted, and a NOTICE file carrying the
// verbatim attribution ships alongside them. Audio2Emotion is licensed
// separately ("use allowed with Audio2Face only") and is deliberately NOT
// fetched — explicit emotion is a user-set slider, never a second model.

#include <QString>
#include <QStringList>

#include <functional>
#include <memory>
#include <vector>

namespace AudioToFace {

/// One solved frame.
struct FaceFrame {
    double timeSec = 0.0;
    std::vector<float> weights;   ///< 52, ARKit order, each [0,1]
    double solveResidual = 0.0;   ///< 0 = the basis reproduced the prediction
};

struct PredictOptions {
    double fps = 30.0;            ///< output frame rate; the window is centred
                                  ///< so any rate is valid for this model
    bool normalise = true;        ///< lift a quiet take (see AudioPreprocess)
    /// Explicit emotion, 10 values in the order listed by network_info.json:
    /// amazement, anger, cheekiness, disgust, fear, grief, joy, outofbreath,
    /// pain, sadness. All-zero is neutral. User-set, never predicted.
    std::vector<float> emotion;
    /// Cancel/progress. Return false to abort; the take returns what it has.
    std::function<bool(int done, int total)> progress;
};

struct PredictResult {
    std::vector<FaceFrame> frames;
    QString error;                ///< non-empty = nothing usable produced
    QString modelVersion;
    /// The progress callback asked to stop. `frames` then holds whatever was
    /// solved before that, which is a TRUNCATED take -- callers must not
    /// commit it as if the run had finished. Reported separately from `error`
    /// because a cancellation is a user decision, not a failure.
    bool cancelled = false;
    bool ok() const { return error.isEmpty() && !frames.empty() && !cancelled; }
};

class A2FPredictor {
public:
    A2FPredictor();
    ~A2FPredictor();
    A2FPredictor(const A2FPredictor&) = delete;
    A2FPredictor& operator=(const A2FPredictor&) = delete;

    /// True when this build can run the model at all.
    static bool available();

    /// Directory the model files live in (`<AppData>/ai_models/a2f/`).
    static QString modelDirectory();

    /// Fetch the model set if absent. Blocking; returns the directory, or an
    /// empty string with `error` set. Honours `QTMESH_A2F_NO_DOWNLOAD`.
    /// True when every model file is already on disk, so a caller can say
    /// "Downloading…" only when it is actually about to download ~320 MB.
    static bool present();
    static QString ensureModelBlocking(QString* error = nullptr);

    /// Load the session and the pose basis. Idempotent.
    bool load(QString* error = nullptr);
    bool isLoaded() const;

    /// The pose names the solve emits, in output order — read from the model
    /// archive rather than assumed, so a future character with a different
    /// set still maps correctly.
    QStringList poseNames() const;

    /// Run a whole take. `samples` is mono or interleaved; `channels` and
    /// `sampleRate` describe it as decoded.
    PredictResult predict(const std::vector<float>& samples,
                          int sampleRate,
                          int channels,
                          const PredictOptions& options = {});

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

}  // namespace AudioToFace

#endif  // AUDIOTOFACE_A2FPREDICTOR_H
