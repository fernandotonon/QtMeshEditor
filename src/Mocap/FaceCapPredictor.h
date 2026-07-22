#ifndef FACECAPPREDICTOR_H
#define FACECAPPREDICTOR_H

// Face capture predictor (epic #869, Slice C #872) — ONNX consumer #9.
// One RGB888 frame in -> 52 blendshape weights + head pose + confidence out,
// via the three MediaPipe graphs converted by scripts/export-facecap-onnx.py
// (Apache-2.0; contract in docs/MOCAP_SPIKE.md):
//   face_detector.onnx     BlazeFace short-range (letterboxed 128, [-1,1])
//   face_landmarks.onnx    Face Mesh V2 (rotated 256 crop, [0,1] -> 478 xyz)
//   face_blendshapes.onnx  MLP-Mixer (146-landmark subset px -> 52 scores)
//
// Detector-skip tracking: once a face is tracked, the next frame's ROI comes
// from the previous frame's landmarks and the detector is skipped until the
// landmark presence drops below threshold (MediaPipe's own strategy).
//
// Models download on first use to AppData/ai_models/mocap/face/ (base URL
// override: QSettings ai/mocapModelBaseUrl or QTMESH_MOCAP_MODEL_BASE_URL;
// offline guard: QTMESH_MOCAP_NO_DOWNLOAD). Without ENABLE_ONNX (implied by
// ENABLE_MOCAP) or without models, isAvailable() stays false and predict()
// returns confidence 0 with lastError() explaining why — never crashes.

#ifdef ENABLE_MOCAP

#include <QImage>
#include <QMetaType>
#include <QString>

#include <array>
#include <memory>

// One predicted sample. Pure data; shared vocabulary with the mapper,
// recorder and GUI is FaceCap::kBlendshapeNames (FaceCapCanonicalData.h).
struct FaceSample {
    double timeSec = 0.0;
    std::array<float, 52> weights{};                     // [0,1] each
    std::array<float, 4> headRotation{0.f, 0.f, 0.f, 1.f};  // (x,y,z,w);
        // +X right, +Y up, camera looks -Z; identity = facing the camera
    std::array<float, 3> headTranslation{0.f, 0.f, 0.f};    // px units
    float confidence = 0.f;                              // 0 = no face
};
Q_DECLARE_METATYPE(FaceSample)

class FaceCapPredictor {
public:
    FaceCapPredictor();
    ~FaceCapPredictor();

    static QString modelDir();       // AppData/ai_models/mocap/face
    static bool modelsPresent();
    // Blocking first-use download of the three graphs (~6.4 MB total).
    // Returns the model dir, or empty on failure/offline-guard/non-ONNX.
    static QString ensureModelsBlocking();

    // Create the ONNX sessions from dir (default: modelDir()).
    bool load(const QString& dir = {});
    bool isAvailable() const;
    QString lastError() const;

    // rgb888: any QImage (converted internally if needed).
    FaceSample predict(const QImage& image, double timeSec);

    void resetTracking();
    // diagnostics: how many frames actually ran the detector (tracking test)
    int detectorRuns() const;

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

#endif  // ENABLE_MOCAP
#endif  // FACECAPPREDICTOR_H
