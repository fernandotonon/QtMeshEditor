#ifndef TRELLIS2_PREDICTOR_H
#define TRELLIS2_PREDICTOR_H

#include "MeshGenPredictor.h"   // shared Result / Stage / ProgressFn contract

#include <QImage>
#include <QString>

// Microsoft TRELLIS.2 image-to-3D backend — the project's highest-quality
// generation tier and the DEFAULT backend whenever its runtime is installed.
//
// TRELLIS.2 ("Native and Compact Structured Latents for 3D Generation",
// arXiv 2512.14692) — **MIT code AND MIT weights** (microsoft/TRELLIS.2 +
// HF microsoft/TRELLIS.2-4B), pinned revisions in ai/trellis2/install.py.
// Unlike the in-process ONNX backends (TripoSR/TripoSG), this one runs as an
// OUT-OF-PROCESS Python sidecar: the 4B-parameter sparse-voxel flow stack
// needs CUDA (Linux + NVIDIA GPU, >=24 GB VRAM recommended) and its custom
// sparse kernels (FlexGEMM/o-voxel/CuMesh, all MIT) have no ONNX lowering.
//
// Division of labour (Phase 5 of the integration plan):
//   Python (ai/trellis2/generate.py) — inference only: image → sparse
//     structure → shape/tex SLats → raw mesh + sparse PBR attribute volume,
//     exported as a QTM3D interchange file (Trellis2Interchange).
//   C++ (this file + Trellis2Bake) — everything that makes an asset: alpha
//     matte via the project's own U²-Net (BackgroundRemover keepAlpha — the
//     upstream default remover briaai/RMBG-2.0 is CC BY-NC and is never
//     loaded), weld/cleanup/simplify (game-ready presets), xatlas UV unwrap,
//     multi-channel PBR texture bake, Ogre build + export.
//
// **NVIDIA nvdiffrast / nvdiffrec are excluded end-to-end** (NVIDIA Source
// Code License — research/evaluation only): not installed, not imported, not
// invoked; the sidecar refuses/warns if they are present, and CI greps for
// them. Full audit: docs/trellis2-dependencies.md.
//
// Runtime discovery (no crash when absent — every surface reports a clean
// "runtime not installed" message): env QTMESH_TRELLIS2_ENV → QSettings
// ai/trellis2Env → <AppData>/trellis2 (where ai/trellis2/install.py installs
// by default). The Python interpreter can be overridden with
// QTMESH_TRELLIS2_PYTHON / QSettings ai/trellis2Python.
class Trellis2Predictor {
public:
    struct Options {
        Options();
        // fast = TRELLIS.2 '512', balanced = '1024_cascade' (upstream
        // default), high = '1536_cascade'.
        QString preset = QStringLiteral("balanced");
        unsigned seed = 42;
        int steps = 0;              // sampler steps override (0 = upstream default)
        // ---- QtMeshEditor-side asset processing --------------------------------
        // Game-ready simplification target; 0 keeps the raw density (Phase 8:
        // ~10k Low / ~25k Medium / ~50k High — no exact-count promise).
        int targetTriangles = 0;
        bool bakeTexture   = true;  // false → per-vertex colours only
        int  textureSize   = 2048;  // 1024 / 2048 / 4096
        int  supersample   = 1;     // 1 or 2 (2 = 2×2 subsamples per texel)
        bool bakeNormalMap = true;  // source detail normals onto the simplified target
        bool removeBackground = true;   // U²-Net alpha matte (skipped if the
                                        // input already carries real alpha)
        // Phase 9: persist the raw generation (QTM3D) here so textures/LODs can
        // be re-baked later without re-running inference. Empty = don't keep.
        QString sourceKeepDir;
        QString sourceKeepBaseName; // file stem for the kept interchange
        // Test hook — run the sidecar's --mock synthetic generation (no GPU).
        bool mock = false;
    };

    // Compiled in unconditionally (no ONNX needed). What actually gates the
    // backend is runtimeAvailable().
    static bool isAvailable();

    // The backend has TWO interchangeable runtime flavors (#966):
    //   TrellisCpp    — the C++/GGML trellis.cpp CLI (CUDA/Vulkan/Metal, no
    //                   Python; invoked with --dump-post so QtMeshEditor keeps
    //                   the game-ready + bake pipeline). Preferred when found.
    //   PythonSidecar — the upstream-exact Python env (ai/trellis2/).
    enum class RuntimeKind { None, TrellisCpp, PythonSidecar };
    static RuntimeKind runtimeKind();

    // ---- Python sidecar flavor ------------------------------------------------
    // Resolved runtime directory ("" when none found).
    static QString runtimeDir();
    // Resolved python interpreter + generate.py ("" when unresolvable).
    static QString pythonPath();
    static QString generateScriptPath();

    // ---- trellis.cpp flavor -----------------------------------------------------
    // Resolved trellis-cli binary: env QTMESH_TRELLIS2_CLI → QSettings
    // ai/trellis2Cli → PATH lookup ("" when unresolvable).
    static QString trellisCliPath();
    // GGUF model dir for the CLI: env QTMESH_TRELLIS2_CLI_MODELS → QSettings
    // ai/trellis2CliModels → <cli dir>/models.
    static QString trellisCliModelsDir();
    // Binary + the minimum 512-pipeline GGUFs present.
    static bool trellisCliAvailable();

    // True when either flavor resolves — the cheap probe every surface gates on.
    static bool runtimeAvailable();
    // One-line human description of the runtime state (for UI/CLI errors).
    static QString runtimeDescription();

    // Run the full TRELLIS.2 pipeline (sidecar inference + native asset
    // processing). Same Result/Stage/ProgressFn contract as
    // MeshGenPredictor::predict: Stage::Encode covers model load/preprocess,
    // Stage::Denoise the generation, Stage::Decode extract/transfer,
    // Stage::Bake the native texture bake. Returning false from `progress`
    // cancels (the sidecar process is terminated). Never throws.
    static MeshGenPredictor::Result predict(
        const QImage& image,
        const Options& opts = {},
        const MeshGenPredictor::ProgressFn& progress = {});

private:
    static QString resolvePython(const QString& dir);
};

#endif // TRELLIS2_PREDICTOR_H
