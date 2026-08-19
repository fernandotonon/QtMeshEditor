#ifndef HANDCAPPREDICTOR_H
#define HANDCAPPREDICTOR_H

// Hand capture predictor — ONNX consumer for MediaPipe Hands (21 landmarks).
// Primary path matches the Hands / Holistic demo: BlazePalm detector (192²)
// → rotated 224² crop → hand_landmarks.onnx. Left crops are mirrored because
// the landmark graph is trained on right hands. Pose-seeded wrist/index/pinky
// ROI is the fallback when the palm detector misses. BlazePose's own tips
// (landmarks 17–22) barely articulate.
//
// Model downloads on first use to AppData/ai_models/mocap/hands/ (same env /
// QSettings overrides as face/pose: QTMESH_MOCAP_MODEL_BASE_URL /
// ai/mocapModelBaseUrl / QTMESH_MOCAP_NO_DOWNLOAD). Missing model → predict()
// returns invalid hands (pose 2D-curl fallback still runs).

#ifdef ENABLE_MOCAP

#include "MocapLiveTypes.h"

#include <QImage>
#include <QString>

#include <memory>

class HandCapPredictor {
public:
    HandCapPredictor();
    ~HandCapPredictor();

    static QString modelDir();
    static bool modelsPresent();
    static QString ensureModelsBlocking();

    bool load(const QString& dir = {});
    bool isAvailable() const;
    QString lastError() const;

    // poseImageXy: 33 × (x,y) pixels in the full frame; poseVis: 33 vis.
    HandsLiveFrame predict(const QImage& image, const float* poseImageXy,
                           const float* poseVis, double timeSec);

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

#endif  // ENABLE_MOCAP
#endif  // HANDCAPPREDICTOR_H
