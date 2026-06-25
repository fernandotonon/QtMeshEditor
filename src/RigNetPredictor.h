#ifndef RIGNET_PREDICTOR_H
#define RIGNET_PREDICTOR_H

#include <QString>
#include <array>
#include <cstdint>
#include <vector>

// RigNet ML skeleton prediction (issue #408), the second ONNX consumer after
// PbrMapSynth (#404). Ogre-free + unit-testable: takes a mesh (positions +
// triangle indices), runs the RigNet ONNX model, and returns predicted joint
// positions with a parent-index hierarchy.
//
// RigNet (Xu et al., SIGGRAPH 2020, "RigNet: Neural Rigging for Articulated
// Characters") predicts joints and bone connectivity from the mesh geometry —
// no template, so it handles arbitrary topology / non-humanoid shapes better
// than the #407 Pinocchio-style template embedding. The full research pipeline
// is multi-stage (a graph attention network over a geodesic-edge mesh graph →
// joint heat-map / attention → clustering → minimum-spanning-tree bone
// connectivity). The shipped model is an ONNX export of that pipeline; this
// class is the runtime that drives it.
//
// **Design contract.** The published RigNet checkpoint is PyTorch-Geometric and
// does not export to a single clean ONNX graph; the export tooling
// (scripts/export-rignet-onnx.py — one-time, offline, NOT shipped) bakes the
// graph construction + the staged network into one model whose I/O this class
// targets. Until that export is hosted, `predict()` reports a graceful failure
// (model missing) and AutoRig falls back to Pinocchio. The whole file is
// `ENABLE_ONNX`-guarded; without it `isAvailable()` is false and `predict()`
// fails with a "rebuild with -DENABLE_ONNX" message.
//
// Coordinate space: positions are mesh-local (the same space AutoRig::fitTemplate
// works in); predicted joints come back in that space. The caller normalises to
// a unit box for the model and de-normalises the result here, so callers don't
// deal with the model's internal scale.
class RigNetPredictor {
public:
    struct Options {
        // Out-of-line ctor so the `{}` default arg on predict() resolves to a
        // constructor call, not class-definition-time aggregate init of this
        // nested struct while RigNetPredictor is still incomplete (which the
        // compiler rejects). Same idiom as AutoRig::Options.
        Options();
        // Target joint budget passed to the model's clustering stage. RigNet's
        // bandwidth/threshold controls joint density; we expose a coarse cap.
        int maxJoints = 32;
        // Up axis: 0=X, 1=Y, 2=Z (default +Y). RigNet is trained +Y-up; a
        // non-Y up axis is rotated into +Y for inference and back out after.
        int upAxis = 1;
    };

    struct Joint {
        QString name;                       // synthesised ("joint_0", "spine_1", …)
        int     parent = -1;                // index into joints (-1 = root)
        std::array<double, 3> pos = {0, 0, 0};   // mesh-local position
    };

    struct Result {
        bool ok = false;
        QString error;                      // populated when !ok
        std::vector<Joint> joints;          // parent-ordered (root first)
    };

    // True only when built with ENABLE_ONNX. (Model presence is checked per
    // call against `modelPath`.)
    static bool isAvailable();

    // Absolute path the RigNet ONNX model is expected at
    // (AppData/ai_models/rignet/rignet.onnx). Same per-user cache convention
    // as the #404 PBR models.
    static QString modelPath();

    // Ensure the model exists on disk, downloading it on first use (blocks via
    // a local event loop, like AIAssistManager::ensureModelBlocking). Returns
    // the model path on success, or empty when offline / disabled / it failed.
    // Honours QTMESH_RIGNET_NO_DOWNLOAD (tests/offline) and the base-URL
    // override QTMESH_RIGNET_MODEL_BASE_URL / QSettings ai/rignetModelBaseUrl.
    static QString ensureModelBlocking();

    // Run RigNet against `modelPath` (an .onnx file). `positions` is tightly
    // packed xyz (3 floats/vertex); `indices` is triangle vertex indices
    // (3/face). Returns predicted joints in mesh-local space, or ok=false with
    // a reason (missing/failed model, degenerate mesh, ONNX-disabled build) so
    // the caller can fall back. Never throws.
    static Result predict(const float* positions, int vertexCount,
                          const uint32_t* indices, int indexCount,
                          const QString& modelPath,
                          const Options& opts = {});
};

#endif // RIGNET_PREDICTOR_H
