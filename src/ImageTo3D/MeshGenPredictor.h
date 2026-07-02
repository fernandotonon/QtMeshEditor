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
// Surface = marching cubes on `density - threshold` at iso 0 (threshold 25.0),
// i.e. MarchingCubes::extract(field = density - threshold, isoLevel = 0) — our MC
// is inside-positive. (TripoSR's own inside-negative MC runs the negated field;
// same surface.)
//
// The whole file is `ENABLE_ONNX`-guarded; without it isAvailable() is false and
// predict() returns a "rebuild with -DENABLE_ONNX" Result. Models live under
// AppData/ai_models/triposr/ and download on first use; the URL is overridable via
// QSettings ai/triposrModelBaseUrl → env QTMESH_TRIPOSR_MODEL_BASE_URL → the hosted
// HF repo, and QTMESH_TRIPOSR_NO_DOWNLOAD forces the offline path.
class MeshGenPredictor {
public:
    // Encoder precision tier. The decoder is tiny and always fp32; only the ~1.7 GB
    // encoder is offered in a smaller quantized variant so users can trade a little
    // quality for a much smaller download (see scripts/export-triposr-onnx.py):
    //   Fp32  — triposr_encoder.onnx        (~1.68 GB, best)
    //   Int8  — triposr_encoder_int8.onnx   (~430 MB, slight quality loss)
    // (fp16 was dropped: TripoSR's attention block has a hardcoded Cast-to-float32
    // that the ONNX fp16 converters can't rewrite cleanly; int8 is smaller anyway.)
    enum class Quality { Fp32, Int8 };

    struct Options {
        Options();                    // out-of-line (same idiom as UniRig::Options)
        int     sdfResolution = 256;  // marching-cubes grid resolution (128 = fast)
        float   threshold     = 25.0f;// TripoSR density iso threshold
        bool    vertexColor   = true; // run the extra color pass on the vertices
        Quality quality       = Quality::Fp32;  // encoder precision tier
        // Run U²-Net background removal on the input first (TripoSR needs an
        // isolated subject). Uses BackgroundRemover; if the model/ONNX is absent
        // the image is used as-is. Recommended for photos; harmless for
        // already-segmented inputs.
        bool  removeBackground = false;
        // Decoder query-point chunk size (points per decoder Run). Bounds memory
        // on the resolution^3 grid; 0 → one shot (only for tiny grids).
        int   chunkPoints   = 262144;

        // ---- Quality pass (post-extraction polish; defaults ON) --------------
        // Taubin λ|μ smoothing removes the marching-cubes stair-stepping
        // without shrinking the model (see MeshRefine). 0 iterations disables.
        bool smoothMesh       = true;
        int  smoothIterations = 6;
        // After smoothing, run one Newton projection step per vertex back onto
        // the network's true iso-surface (field + gradient sampled from the
        // decoder at the smoothed positions) — recovers detail the MC grid
        // quantized away and undoes any residual smoothing drift.
        bool refineSurface = true;
        // Bake a real diffuse TEXTURE via xatlas unwrap + per-texel decoder
        // colour queries (MeshGenBaker) instead of per-vertex colours. Colour
        // sharpness then scales with textureSize, not with vertex density.
        // Requires vertexColor-style colour output on the decoder; falls back
        // to vertex colours (Result::warning set) if the unwrap/bake fails.
        bool bakeTexture = true;
        int  textureSize = 1024;
    };

    struct Result {
        bool ok = false;
        QString error;                 // populated when !ok
        QString warning;               // non-fatal (e.g. bake fell back to vertex colours)
        std::vector<float>    positions; // Nx3, world space
        std::vector<uint32_t> indices;   // Mx3
        std::vector<float>    colors;    // Nx3 in [0,1], empty if not generated
        // Baked-texture path (Options::bakeTexture): UV0 per vertex + the baked
        // diffuse image. Both empty/null when the bake was disabled or fell back.
        std::vector<float>    uvs;       // Nx2 in [0,1]
        QImage                texture;
        int vertexCount   = 0;
        int triangleCount = 0;
        bool usedModel    = false;     // true iff the ONNX path ran
    };

    // True only when built with ENABLE_ONNX. (Model presence is checked per call.)
    static bool isAvailable();

    // AppData/ai_models/triposr/ paths for the two graphs. The encoder path
    // depends on the quality tier; the no-arg overloads default to Fp32 for
    // existing callers.
    static QString encoderModelPath(Quality q = Quality::Fp32);
    static QString decoderModelPath();
    static QString modelPath();             // == encoderModelPath(Fp32) (convenience)
    // Bare filename of the encoder for a tier (for download labels / AI Settings).
    static QString encoderFileName(Quality q);

    // True when the decoder AND the given tier's encoder already exist on disk.
    static bool modelsPresent(Quality q = Quality::Fp32);

    // Ensure the decoder + the given tier's encoder exist, downloading whichever
    // is missing on first use (blocks via a local event loop, like
    // UniRigPredictor::ensureModelBlocking). Returns the encoder path when both are
    // present, else empty (offline / disabled / download failed / not-yet-hosted).
    // Honours QTMESH_TRIPOSR_NO_DOWNLOAD + the base-URL override.
    static QString ensureModelBlocking(Quality q = Quality::Fp32);

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
