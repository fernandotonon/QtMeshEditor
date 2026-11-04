#ifndef TRIPOSPLAT_PREDICTOR_H
#define TRIPOSPLAT_PREDICTOR_H

#include "MeshGenPredictor.h"   // shared Stage / ProgressFn contract
#include "SplatToMesh.h"

#include <QImage>
#include <QString>

#include <functional>
#include <vector>

// TripoSplat single-image -> 3D Gaussians backend (`--backend triposplat`).
//
// TripoSplat (VAST-AI, 2026, "Generative 3D Gaussians with Learned Density
// Control", arXiv 2605.16355 — MIT code AND MIT weights on HF
// `VAST-AI/TripoSplat`) generates a variable number of 3D Gaussians from one
// image. Bundled third-party weights: DINOv3 ViT-H (Meta DINOv3 License —
// commercial use allowed; a copy of the licence must accompany the weights,
// so LICENSE-DINOv3.txt downloads next to the graphs) and the FLUX.2 VAE
// encoder (Apache-2.0). See THIRD_PARTY_AI_MODELS.md.
//
// Native runtime (no Python): five ONNX graphs exported by
// scripts/export-triposplat-onnx.py (contract documented there):
//   dinov3  image[1,3,1024,1024] -> feature1[1,4101,1280]
//   vae     image + noise[1,32,128,128] -> feature2[1,4101,128]
//   flow    one Euler step (latent[B,8192,16], camera[B,1,5], t*1000,
//           feature1, feature2) -> velocities
//   octree  coarse-to-fine point placement logits (8 levels, 8 children)
//   gs      points[1,P,3] + latent -> 32 activated Gaussians per point
// The flow-matching Euler loop with classifier-free guidance, the octree
// systematic sampling and the splat -> mesh conversion (SplatToMesh) run
// here. All attention runs as com.microsoft.MultiHeadAttention (fused on
// CPU: the unfused export needed ~10 GB per flow call).
//
// Speed (CPU, Apple M-series, ORT 1.31): ~38 s per flow call, i.e. ~25 min
// at the reference 20 steps with guidance. Guidance cannot be dropped to
// save time: without it some seeds produce scattered fragments (measured).
// CoreML cannot compile the flow graph, so macOS stays on the CPU; the CUDA
// EP is used on Linux when available.
//
// Whole runtime is ENABLE_ONNX-gated; models download on first use to
// AppData/ai_models/triposplat/ (QTMESH_TRIPOSPLAT_MODEL_BASE_URL /
// QSettings ai/triposplatModelBaseUrl; QTMESH_TRIPOSPLAT_NO_DOWNLOAD guard).
// QTMESH_TRIPOSPLAT_FIXTURE=<npz> replays the random draws written by
// scripts/verify_triposplat_native.py --fixture, for C++/Python parity.
class TripoSplatPredictor {
public:
    // Surface defaults (CLI / MCP / GUI) when the caller sets none. 1024^3
    // matches the splat spacing (median 0.00101 of the unit box at 262k
    // Gaussians vs a 0.00103 voxel); finer grids add triangles, not shape.
    // The dense 1.7M-triangle mesh is then the SOURCE of a game-ready
    // simplification + detail normal map — unwrapping it directly takes
    // xatlas many minutes. Measured on an M-series CPU: 77 s, 3.6 GB peak
    // for the mesh + bake stages at these defaults.
    static constexpr int kDefaultMeshResolution  = 1024;
    static constexpr int kDefaultTargetTriangles = 150000;
    static constexpr int kDefaultTextureSize     = 2048;

    struct Options {
        Options();
        int   flowSteps     = 20;       // Euler steps (reference 20; 10 is a preview)
        float guidanceScale = 3.0f;     // CFG; <= 1 disables (unreliable, see above)
        float shift         = 3.0f;     // timestep schedule shift
        int   numGaussians  = 262144;   // 32768..262144, multiple of 32
        unsigned seed       = 42;
        // Matte the subject before inference. The model is trained on a subject
        // composited over BLACK; leaving a background in makes it part of the
        // object. When true (the GUI "Remove background" checkbox / CLI
        // --remove-bg), ALWAYS run the matte — even if the input already has
        // some alpha (a soft edge or a partial cutout is not a subject matte).
        // When false, still matte inputs with no real alpha so a plain photo
        // does not silently become a room slab.
        bool removeBackground = true;
        // Best (BiRefNet) is the generation default; Fast is the explicit opt-out.
        BackgroundRemover::Quality mattingQuality = BackgroundRemover::Quality::Best;
        // Splat -> mesh voxel resolution and post-extraction smoothing.
        int  meshResolution   = 256;
        bool smoothMesh       = true;
        int  smoothIterations = 4;
    };

    // Output: the mesh in the EDITOR frame (Y up, facing +Z), plus the
    // splats in the same frame for colour sampling / the .ply side output.
    struct Output {
        MeshGenPredictor::Result mesh;
        SplatToMesh::Splats splatsModelFrame;  // as generated (for the .ply)
        std::vector<float>  splatPositionsEditor; // Nx3, editor frame
        std::vector<float>  splatColors;          // Nx3 linear [0,1]
        float voxelSize = 0.0f;                   // editor units
    };

    static bool isAvailable();

    static QString modelDir();
    static QStringList modelFiles();          // every file the runtime needs
    static bool modelsPresent();
    // Download any missing file and verify every graph against its pinned
    // SHA-256 (default hosting; skipped while a marker records an earlier
    // verification at the same size + mtime), plus the licence texts that
    // must travel with the weights. Blocking — call on a thread with an event
    // loop (NOT a worker). `cancelled` is polled during multi-GB transfers.
    static bool ensureModelsBlocking(QString* error = nullptr,
                                     const std::function<bool()>& cancelled = {});
    // Resolved download base (QSettings ai/triposplatModelBaseUrl ->
    // QTMESH_TRIPOSPLAT_MODEL_BASE_URL -> the hosted repo), ending in '/'.
    static QString baseUrl();
    static bool isDefaultHosting(const QString& base);
    // Pinned SHA-256 of a hosted file ("" for an unknown name).
    static QString expectedSha256(const QString& fileName);
    static QStringList licenseFiles();

    // Full pipeline: matte (if needed) -> prepare -> generate -> mesh.
    static Output predict(const QImage& image, const Options& opts = {},
                          const MeshGenPredictor::ProgressFn& progress = {});

    // ---- Pure helpers (unit-tested) ----------------------------------------
    // Upstream preprocess_image after the matte: erode the alpha by one pixel,
    // crop a square 1.2x the alpha bounding box around its centre (out of
    // bounds = transparent), resize to 1024 and composite over black. An
    // image without any alpha is cropped around its full extent.
    static QImage prepareImage(const QImage& rgba);
    // Flow-matching timestep schedule with shift: t_i = s*l/(1+(s-1)*l),
    // l = linspace(1, 0, steps+1).
    static std::vector<double> timeSchedule(int steps, double shift);
    // sample_probs(algo='systematic'): split `count` samples over `probs`
    // (8 children) with one uniform u in [0,1): positions (u + k)/count.
    static void systematicCounts(const float* probs, int count, double u, int out[8]);

private:
    static Output meshSplats(Output out, const Options& opts,
                             const MeshGenPredictor::ProgressFn& progress,
                             const QString& warning);
};

#endif // TRIPOSPLAT_PREDICTOR_H
