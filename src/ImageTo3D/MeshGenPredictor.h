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

    // Generation backend. TripoSR = the fast single-pass LRM;
    // TripoSG = the 1.5B rectified-flow model (higher-fidelity geometry,
    // slower, geometry-only — see TripoSGPredictor). Both MIT code+weights.
    // Trellis2 = Microsoft TRELLIS.2 (MIT code+weights) via the out-of-process
    // Python sidecar (ai/trellis2/, Linux + NVIDIA GPU) — the highest-quality
    // tier and the DEFAULT whenever its runtime is installed (see
    // Trellis2Predictor + defaultBackend()); mesh cleanup/UVs/PBR baking are
    // done natively by Trellis2Bake, deliberately without NVIDIA
    // nvdiffrast/nvdiffrec (docs/trellis2-dependencies.md).
    enum class Backend { TripoSR, TripoSG, Trellis2 };

    // The backend a surface should preselect when the user didn't choose one:
    // Trellis2 when its runtime is available on this machine, else TripoSR.
    static Backend defaultBackend();

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

        // ---- Backend selection ------------------------------------------------
        // TripoSG ignores the colour/bake options (geometry-only model) and
        // maps Quality::Int8 onto its int8 DiT tier. flowSteps/guidanceScale
        // only apply to TripoSG.
        Backend backend = Backend::TripoSR;
        int   flowSteps = 25;
        float guidanceScale = 7.0f;

        // ---- TRELLIS.2-only options (Backend::Trellis2) -----------------------
        unsigned seed = 42;                 // deterministic generation seed
        QString  trellis2Preset =           // fast | balanced | high
            QStringLiteral("balanced");
        // Game-ready simplification target (Phase 8 presets: Low ~10k /
        // Medium ~25k / High ~50k). 0 = keep the original TRELLIS.2 density.
        int  targetTriangles = 0;
        // Bake a tangent-space normal map carrying the full-res source detail
        // (only meaningful when the target was simplified; needs bakeTexture).
        bool bakeNormalMap = true;
        // Test hook: drive the sidecar's --mock synthetic generation (no GPU,
        // no TRELLIS.2 models) — used by the plumbing e2e tests.
        bool trellis2Mock = false;
        // Phase 9: where to persist the raw generation (QTM3D interchange) so
        // textures/LODs can be re-baked later without re-running inference.
        // Empty = don't keep. The kept path lands in Result::sourceInterchangePath.
        QString trellis2SourceKeepDir;
        QString trellis2SourceKeepBaseName;
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
        // Optional precomputed smooth shading normals (Nx3). When present,
        // MeshGenBuilder uses them instead of recomputing from the (possibly
        // seam-split) index buffer — the TRELLIS.2 bake provides
        // position-welded ones so chart seams stay smooth.
        std::vector<float>    normals;
        QImage                texture;
        int vertexCount   = 0;
        int triangleCount = 0;
        bool usedModel    = false;     // true iff the ONNX path ran
        // TripoSR's reconstruction frame lies on its back + faces 90° off, so
        // MeshGenBuilder bakes a fixed -90°X/+90°Y into the vertex data.
        // TripoSG's field is already +Y-up (upstream exports the marching-cubes
        // trimesh as-is), so its dispatch sets this false to skip the bake.
        bool bakeTripoSROrientation = true;

        // ---- TRELLIS.2 extras (empty/null for the other backends) -------------
        // Real baked PBR maps from the sparse attribute volume (Trellis2Bake).
        // When present, MeshGenBuilder binds them into the canonical
        // normal_map/roughness/metallic slots and SKIPS the #404 PbrMapSynth
        // guess-from-albedo chain.
        QImage normalMap;      // tangent-space, OpenGL +Y up
        QImage roughnessMap;   // grayscale
        QImage metallicMap;    // grayscale
        // Phase 9: the preserved full-resolution generation (QTM3D interchange)
        // so textures/LODs can be re-baked later without re-running inference.
        QString sourceInterchangePath;
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

    // Pipeline stage identifiers for the progress callback — one per
    // user-visible step of predict() (the GUI shows a per-step progress list).
    enum class Stage {
        Encode,   // image encoder run (single blocking call: 0/1 → 1/1)
        Denoise,  // TripoSG rectified-flow Euler loop (per-step)
        Decode,   // res³ grid decode (per-chunk; the long one)
        Refine,   // iso-surface reprojection probes (per-chunk)
        Bake,     // texture bake colour queries (per-chunk, baker-reported)
        Color,    // per-vertex colour fallback pass (per-chunk)
    };

    // Progress/cancel callback. Invoked per unit of work with
    // (stage, done, total); return false to CANCEL (predict then returns
    // ok=false, error="cancelled"). May also be invoked with total <= 0 as a
    // pure CANCELLATION CHECK — treat that as "no bar update".
    using ProgressFn = std::function<bool(Stage stage, int done, int total)>;

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
