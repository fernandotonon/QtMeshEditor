#ifndef POSECAPPREDICTOR_H
#define POSECAPPREDICTOR_H

// Body-pose predictor (epic #869, Slice E #874) — ONNX consumer #10.
// One RGB888 frame in -> 33 MediaPipe world landmarks (metres, hip-centred)
// + visibility + confidence out, via the two graphs converted by
// scripts/export-facecap-onnx.py --pose (Apache-2.0; contract in
// docs/MOCAP_SPIKE.md): pose_detector.onnx (letterboxed 224, [-1,1], 2254
// anchors) and pose_landmarks.onnx (rotated 256 crop, [0,1] -> 39x5 screen
// landmarks + world landmarks + presence).
//
// Detector-skip tracking mirrors FaceCapPredictor: the next frame's ROI comes
// from this frame's auxiliary alignment landmarks (raw landmarks 33/34 — the
// mid-hip centre + scale point BlazePose emits exactly for this purpose).
//
// Models download on first use to AppData/ai_models/mocap/pose/ (same env /
// QSettings overrides as the face models: QTMESH_MOCAP_MODEL_BASE_URL /
// ai/mocapModelBaseUrl / QTMESH_MOCAP_NO_DOWNLOAD).

#ifdef ENABLE_MOCAP

#include <QImage>
#include <QString>

#include <array>
#include <memory>

struct PoseSample {
    double timeSec = 0.0;
    std::array<float, 33 * 3> world{};      // xyz metres, MediaPipe world frame
    /// Normalized x,y in the 256 crop [0,1]; z is model relative depth.
    std::array<float, 33 * 3> screenCrop{};
    /// Full-frame pixel x,y (LandmarkProjection through the pose ROI).
    std::array<float, 33 * 2> imageXy{};
    std::array<float, 33> visibility{};     // sigmoided per-landmark visibility
    float confidence = 0.f;                 // pose presence; 0 = none found
};

class PoseCapPredictor {
public:
    PoseCapPredictor();
    ~PoseCapPredictor();

    static QString modelDir();  // AppData/ai_models/mocap/pose
    static bool modelsPresent();
    static QString ensureModelsBlocking();

    bool load(const QString& dir = {});
    bool isAvailable() const;
    QString lastError() const;

    PoseSample predict(const QImage& image, double timeSec);
    void resetTracking();
    int detectorRuns() const;

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

#endif  // ENABLE_MOCAP
#endif  // POSECAPPREDICTOR_H
