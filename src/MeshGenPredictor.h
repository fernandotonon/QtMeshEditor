#ifndef MESH_GEN_PREDICTOR_H
#define MESH_GEN_PREDICTOR_H

#include <QImage>
#include <QString>
#include <cstdint>
#include <functional>
#include <vector>

// TripoSR single-image → 3D mesh generation (epic #764, slice B #766).
// The FIFTH ONNX consumer, after PbrMapSynth (#404), UniRig (#408),
// MotionInbetween (#409) and MeshSegmenter (#410). Ogre-free + unit-testable:
// takes a QImage, runs the two exported TripoSR ONNX graphs, extracts the surface
// with the native marching cubes (src/MarchingCubes.*), and returns raw
// vertex/index (+ optional vertex-color) arrays. Slice C (#767) turns those into
// an Ogre::Mesh; this class does NO Ogre work.
//
// TripoSR (Tripo AI + Stability AI, "TripoSR: Fast 3D Object Reconstruction from
// a Single Image", arXiv 2403.02151 — MIT code AND MIT weights on HF
// `stabilityai/TripoSR`) reconstructs a NeRF-style triplane from one image. MIT
// code+weights is the deciding factor (redistributable via Homebrew/Snap/WinGet/
// Docker) — the same bar UniRig #408 cleared and non-commercial SF3D failed. See
// THIRD_PARTY_AI_MODELS.md and docs/IMAGE_TO_3D_SPIKE_764.md.
//
// **Two ONNX graphs** (exported by scripts/export-triposr-onnx.py; contract fixed
// by the slice-A spike):
//   * encoder.onnx: image [1,3,512,512] (RGB in [0,1], /255 only — NO mean/std)
//     → scene_codes triplane [1,3,40,64,64].
//   * decoder.onnx (per-point): scene_codes + points [1,P,3] (world coords in
//     (-radius,radius), radius=0.87) → density [1,P,1] (post density_act) and
//     color [1,P,3]. The grid is tiled through this in chunks.
// Surface = marching cubes on `-(density - threshold)` at iso 0 (threshold 25.0),
// i.e. MarchingCubes::extract(field = density - threshold, isoLevel = 0).
//
// The whole file is `ENABLE_ONNX`-guarded; without it isAvailable() is false and
// predict() returns a "rebuild with -DENABLE_ONNX" Result. Models live under
// AppData/ai_models/triposr/ and download on first use; the URL is overridable via
// QSettings ai/triposrModelBaseUrl → env QTMESH_TRIPOSR_MODEL_BASE_URL → the hosted
// HF repo, and QTMESH_TRIPOSR_NO_DOWNLOAD forces the offline path.
class MeshGenPredictor {
public:
    struct Options {
        Options();                    // out-of-line (same idiom as UniRig::Options)
        int   sdfResolution = 256;    // marching-cubes grid resolution (128 = fast)
        float threshold     = 25.0f;  // TripoSR density iso threshold
        bool  vertexColor   = true;   // run the extra color pass on the vertices
        // Run U²-Net background removal on the input first (TripoSR needs an
        // isolated subject). Uses BackgroundRemover; if the model/ONNX is absent
        // the image is used as-is. Recommended for photos; harmless for
        // already-segmented inputs.
        bool  removeBackground = false;
        // Decoder query-point chunk size (points per decoder Run). Bounds memory
        // on the resolution^3 grid; 0 → one shot (only for tiny grids).
        int   chunkPoints   = 262144;
    };

    struct Result {
        bool ok = false;
        QString error;                 // populated when !ok
        std::vector<float>    positions; // Nx3, world space
        std::vector<uint32_t> indices;   // Mx3
        std::vector<float>    colors;    // Nx3 in [0,1], empty if not generated
        int vertexCount   = 0;
        int triangleCount = 0;
        bool usedModel    = false;     // true iff the ONNX path ran
    };

    // True only when built with ENABLE_ONNX. (Model presence is checked per call.)
    static bool isAvailable();

    // AppData/ai_models/triposr/ paths for the two graphs.
    static QString encoderModelPath();
    static QString decoderModelPath();
    static QString modelPath();             // == encoderModelPath() (convenience)

    // True when BOTH model files already exist on disk.
    static bool modelsPresent();

    // Ensure both models exist, downloading whichever is missing on first use
    // (blocks via a local event loop, like UniRigPredictor::ensureModelBlocking).
    // Returns the encoder path when both are present, else empty (offline /
    // disabled / download failed / not-yet-hosted). Honours
    // QTMESH_TRIPOSR_NO_DOWNLOAD + the base-URL override.
    static QString ensureModelBlocking();

    // Progress/cancel callback for the (long) grid query. Invoked per decoder
    // chunk with (pointsDone, pointsTotal); return false to CANCEL (predict then
    // returns ok=false, error="cancelled").
    using ProgressFn = std::function<bool(int pointsDone, int pointsTotal)>;

    // Run TripoSR against the two .onnx files. `image` is the input photo (any
    // format; converted to RGB and resized to the encoder's 512² internally).
    // Returns vertex/index (+ color) arrays in a centred, roughly unit-scale
    // space, or ok=false with a reason (missing/failed model, ONNX-disabled build,
    // empty surface, cancelled). Never throws.
    static Result predict(const QImage& image,
                          const QString& encoderModelPath,
                          const QString& decoderModelPath,
                          const Options& opts = {},
                          const ProgressFn& progress = {});

    // ---- Pure-data helpers (no ONNX / no Ogre — unit-testable) ----------------

    // Build the resolution^3 query-point grid TripoSR expects: points in
    // (-radius, radius) laid out to match the density-grid indexing predict()
    // uses when it calls MarchingCubes (x fastest, matching row-major
    // field[z*n*n + y*n + x]). Returned tightly packed xyz (3 floats/point),
    // count = res^3. Exposed for tests + so slice B's grid fill and the MC layout
    // provably agree.
    static std::vector<float> buildGridPoints(int resolution, float radius);
};

#endif // MESH_GEN_PREDICTOR_H
