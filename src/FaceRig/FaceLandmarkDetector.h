#ifndef FACELANDMARKDETECTOR_H
#define FACELANDMARKDETECTOR_H

// Facial-landmark detection for the face auto-rig (#889). Runs the MediaPipe
// Face Mesh V2 landmark graph (face_landmarks.onnx — Apache-2.0, the same model
// the mocap face-capture uses, #869) on a rendered head image to get the 478
// canonical face landmarks. Those anchor the non-rigid ICP fit so the ARKit
// template lands on the ACTUAL face features (eyes/nose/mouth) instead of a
// low-residual-but-mis-oriented drape — the fix for wrong shape placement.
//
// We render the head ourselves (centred, front-facing, evenly lit), so the
// upstream face DETECTOR is unnecessary — we feed a plain centred 256×256 crop
// straight to the landmark graph. Ogre-free + ENABLE_ONNX-guarded; the model
// downloads on first use to ai_models/facerig/ (same dir/base-url as the ARKit
// template). Without ONNX or the model, isAvailable() stays false and the
// caller falls back to the landmark-free fit.

#include <QImage>
#include <QString>

#include <array>
#include <memory>
#include <vector>

namespace FaceRig {

struct LandmarkResult {
    // 478 landmarks as (x,y,z), x/y in the INPUT IMAGE's pixel space (0..W/H),
    // z a relative depth (unused for back-projection). Empty when no face.
    std::vector<std::array<float, 3>> points;
    float confidence = 0.0f;   // presence sigmoid, 0 = no face
    // RAW presence logit. The sigmoid saturates (~1.0) for both a true face
    // (logit ~+20) and a convincing false positive like the smooth back of a
    // head (low positive logit) — rank candidate views/crops by THIS, never
    // by `confidence`.
    float presenceLogit = -1e9f;
    bool ok = false;
};

class FaceLandmarkDetector {
public:
    FaceLandmarkDetector();
    ~FaceLandmarkDetector();

    // ---- model management (mirrors ArkitTemplate) ----
    static QString modelPath();          // AppData/ai_models/facerig/face_landmarks.onnx
    static bool present();
    // Blocking first-use download; returns the path or empty (offline guard
    // QTMESH_FACERIG_NO_DOWNLOAD; base URL QTMESH_FACERIG_MODEL_BASE_URL /
    // QSettings ai/facerigModelBaseUrl — same as the ARKit template).
    static QString ensureModelBlocking();

    // True only when built with ENABLE_ONNX.
    static bool backendAvailable();

    // Load the ONNX session from `path` (default: modelPath()). Returns false
    // if ONNX is off / the file is missing / the session can't be created.
    bool load(const QString& path = {});
    bool isAvailable() const;
    QString lastError() const { return m_error; }

    // Detect landmarks on `image` (any format; the head should fill the frame,
    // centred and front-facing). Points come back in `image`'s pixel space.
    // Runs TWO passes: a loose full-frame pass to locate the face, then a
    // tight re-crop around the pass-1 landmark bbox — FaceMesh is trained on
    // detector-cropped faces, so a loose frame depresses both the presence
    // score and landmark accuracy.
    LandmarkResult detect(const QImage& image);

private:
    // One inference on the square crop (ox, oy, side) of `image`; landmark
    // x/y mapped back to `image` pixel space.
    LandmarkResult runPass(const QImage& image, int ox, int oy, int side);

    struct Impl;
    std::unique_ptr<Impl> d;
    QString m_error;
};

}  // namespace FaceRig

#endif  // FACELANDMARKDETECTOR_H
