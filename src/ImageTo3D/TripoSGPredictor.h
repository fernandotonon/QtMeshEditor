#ifndef TRIPOSG_PREDICTOR_H
#define TRIPOSG_PREDICTOR_H

#include "MeshGenPredictor.h"   // shared Result / Stage / ProgressFn contract

#include <QImage>
#include <QString>

// TripoSG single-image → 3D geometry backend (follow-up to epic #764).
//
// TripoSG (VAST-AI-Research, SIGGRAPH 2025, "TripoSG: High-Fidelity 3D Shape
// Synthesis using Large-Scale Rectified Flow Models" — MIT code AND MIT
// weights on HF `VAST-AI/TripoSG`) is a 1.5B rectified-flow DiT over an SDF
// VAE's vecset latents. Reported geometry quality ≈ commercial Tripo 2.0
// (Normal-FID 5.81 vs ~20 for TripoSR-class LRMs) — a full generation ahead
// of the TripoSR fast tier. Same org whose UniRig (#408) we already ship;
// license audit in docs/IMAGE_TO_3D_QUALITY.md.
//
// **Four ONNX graphs** (exported by scripts/export-triposg-onnx.py; the full
// measured contract lives in docs/TRIPOSG_EXPORT_NOTES.md):
//   * triposg_image_encoder.onnx — image [1,3,224,224] (raw [0,1] RGB; the
//     DINOv2-large mean/std preprocessing is baked into the graph) →
//     image_embeds [1,257,1024]. CFG's unconditional embedding is simply
//     ZEROS of the same shape (upstream uses zeros_like), so no second output.
//   * triposg_dit_step.onnx (+ .onnx.data sidecar — the fp32 graph is >2 GB;
//     int8 variant triposg_dit_step_int8.onnx is a single smaller file) —
//     ONE denoising step: (latents[B,2048,64], timestep[B]=1000·σ,
//     image_embeds[B,257,1024]) → velocity[B,2048,64]. The rectified-flow
//     Euler loop runs HERE in C++ (same hand-rolled-loop stance as UniRig's
//     KV-cache decode): σᵢ = 1 − i/N, σ_N = 0, and the update is
//     latents += (σᵢ − σᵢ₊₁)·v  — note the sign is the OPPOSITE of diffusers'
//     stock FlowMatchEuler (TripoSG's custom RectifiedFlowScheduler).
//   * triposg_vae_latents.onnx — latents → kv_cache [1,2048,1024]; the VAE's
//     16-block latent self-attention stack, run ONCE per generation so the
//     per-chunk decode below doesn't re-pay it (~340 chunks at 257³).
//   * triposg_vae_decoder.onnx — (kv_cache, points [1,P,3]) → field [1,P,1],
//     already negated to our INSIDE-POSITIVE convention, iso 0, bounds
//     (−1.005, 1.005)³. Tiled through in chunks exactly like the TripoSR
//     decoder; surface extracted with the SAME native MarchingCubes.
//
// TripoSG is GEOMETRY-ONLY (no colour decoder): the texture-bake stage is
// skipped and the result carries no colours/uvs — pair it with the input-image
// projection follow-up for colour. The smoothing/reprojection quality passes
// apply unchanged (the field sampler is the VAE decoder).
//
// Whole file is ENABLE_ONNX-gated like MeshGenPredictor; models download on
// first use from the hosted models repo (override QTMESH_TRIPOSG_MODEL_BASE_URL
// / QSettings ai/triposgModelBaseUrl; offline guard QTMESH_TRIPOSG_NO_DOWNLOAD)
// and every surface reports a clean "not yet hosted" error when absent.
class TripoSGPredictor {
public:
    struct Options {
        Options();
        int   sdfResolution = 256;    // marching-cubes grid resolution
        // Rectified-flow Euler steps. TripoSG's reference inference uses 50;
        // rectified flow degrades gracefully at lower counts — 25 is a good
        // CPU default, 10 a fast preview.
        int   flowSteps = 25;
        // Classifier-free guidance scale (reference default 7.0). 0 disables
        // the unconditional pass (halves DiT cost, softer geometry).
        float guidanceScale = 7.0f;
        // Deterministic latent seed — same image + params → same mesh.
        unsigned seed = 42;
        // Use the int8-quantized DiT step graph (~1.5 GB single file) instead
        // of fp32 (~5.8 GB as .onnx + .onnx.data) — the TripoSR-style
        // size/quality tier.
        bool useInt8Dit = false;
        // Post-extraction polish (same semantics as MeshGenPredictor).
        bool smoothMesh       = true;
        int  smoothIterations = 6;
        bool refineSurface    = true;
        // Decoder query-point chunk size (bounds memory on the res³ grid).
        // MUCH smaller than TripoSR's (262144): TripoSG's decoder is a
        // CROSS-ATTENTION from every query point to the 2048 kv tokens, so
        // per-Run activation memory scales with P × 2048 × heads — 256k
        // points/chunk materialised ~90 GB of attention logits and got the
        // process killed. 8192 keeps the transient under a few hundred MB.
        // predict() clamps to this cap regardless of what the caller passes.
        int  chunkPoints = 8192;
    };

    // True only when built with ENABLE_ONNX.
    static bool isAvailable();

    // AppData/ai_models/triposg/ paths for the graphs.
    static QString imageEncoderPath();
    static QString ditStepPath(bool int8Tier = false);
    static QString vaeLatentsPath();
    static QString vaeDecoderPath();

    // True when every graph of the chosen DiT tier exists on disk (for the
    // fp32 tier this includes the .onnx.data external-weights sidecar).
    static bool modelsPresent(bool int8Tier = false);

    // Ensure all graphs exist, downloading any missing one on first use
    // (blocks via a local event loop — call on a thread WITH an event loop).
    // Returns the encoder path when everything is present, else empty.
    static QString ensureModelBlocking(bool int8Tier = false);

    // Run the full TripoSG pipeline. Same Result/Stage/ProgressFn contract as
    // MeshGenPredictor::predict (Stage::Encode covers the image encoder,
    // Stage::Denoise the flow loop, Stage::Decode the grid decode,
    // Stage::Refine the reprojection pass). Never throws.
    static MeshGenPredictor::Result predict(
        const QImage& image,
        const Options& opts = {},
        const MeshGenPredictor::ProgressFn& progress = {});
};

#endif // TRIPOSG_PREDICTOR_H
