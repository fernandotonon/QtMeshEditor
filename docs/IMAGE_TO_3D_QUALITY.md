# Image-to-3D quality roadmap (post-#764)

Goal: close the visible gap between our local TripoSR output and commercial
services (Meshy, Tripo3D) while staying permissive-license-clean and local.

## Shipped: the quality pass (this branch)

Three classical post-processing stages, ON by default, between marching cubes
and mesh construction (`MeshGenPredictor::Options {smoothMesh, refineSurface,
bakeTexture, textureSize}`):

1. **Taubin smoothing** (`MeshRefine::taubinSmooth`) — λ|μ alternating Laplacian
   (λ=0.5, μ=-0.53, 6 iterations). Removes the res³-grid stair-stepping that
   reads as "AI blob" without the volume shrinkage of plain Laplacian.
2. **Iso-surface reprojection** (`MeshRefine::isoProjectStep`) — one Newton step
   per vertex toward the decoder's true zero level set, using forward-difference
   gradients from 4 extra decoder probes per vertex (ε = half a grid cell, step
   clamped to one cell). Restores detail the MC grid quantized away and undoes
   residual smoothing drift. Cost: nv×4 extra decoder queries (~1 grid chunk).
3. **Diffuse texture bake** (`MeshGenBaker`) — xatlas auto-unwrap, UV-space
   triangle rasterization (barycentric texel → 3D surface point), per-texel
   decoder colour queries, chart-border dilation. Output: UV0 + a real texture
   (default 1024², atlas may grow to fit). Replaces per-vertex colour, whose
   effective resolution was capped by MC vertex density AND which didn't
   survive export to most viewers (rendered flat white). The baked texture
   survives every export path (`MeshGenBuilder` saves the PNG + registers a
   resource location; the CLI lands it next to the output mesh).

Verified end-to-end on macOS: baseline (no pass) renders as a white unlit blob
through the glb → turntable round-trip; the quality output renders textured and
smooth. ~30 s total at res 192 + 1024² bake on an M-series laptop (338% CPU).

Opt-outs: CLI `--no-smooth --no-refine --no-bake-texture --texture-size N`,
MCP `smooth/refine/bake_texture/texture_size`, GUI inherits defaults.

## Next: candidate upgrades (researched 2026-07, licenses verified)

Ranked by (quality gain × feasibility ÷ license risk). Full license audit in
the table below.

### 1. TripoSG as a second, higher-quality geometry backend (RECOMMENDED)
- VAST-AI `TripoSG` (SIGGRAPH 2025): **MIT code + MIT weights** (verified HF
  card + repo LICENSE) — the only 2025-class quality jump that is fully
  license-clean end-to-end. Same org as UniRig (#408).
- Rectified-flow DiT (1.5B) + SDF VAE. Architecture maps onto our existing
  ONNX pattern: DINOv2 image conditioning (export as one graph), the DiT step
  as a second graph driven by a C++ flow loop (we already hand-roll UniRig's
  autoregressive decode), and a query-points→SDF cross-attention decoder that
  is nearly identical to our chunked TripoSR decode. Our MC already handles
  SDF fields (inside-positive, iso 0).
- Reported quality ≈ commercial Tripo 2.0 (Normal-FID 5.81 vs ~20 for
  TripoSR-class LRMs). Geometry only → reuse this branch's bake for colour...
  but TripoSG has no colour decoder, so texture comes from input-image
  projection (see 3) or a texture model.
- Cost: ~3 GB fp16 download (int8-quantize like the TripoSR tiers), minutes of
  CPU inference (expose a steps knob; rectified flow tolerates few steps).
  No community ONNX export exists — `scripts/export-triposg-onnx.py` is an
  export effort comparable to the UniRig one (3 graphs + flow loop).
- UX: `--backend triposr|triposg` (keep TripoSR as the fast/preview tier),
  mirroring the fp32/int8 tier picker.

### 2. Cheap wins still on the table for the TripoSR path
- **Real-ESRGAN upscale of the baked texture** (infra already shipped, #405):
  bake at 1024 → upscale 2× → sharper texture for ~8 s extra. One flag + one
  call into `TextureUpscaler`.
- **Input-image front-projection blend**: for texels whose surface normal faces
  the input camera, blend the actual input pixels over the decoder colour
  (weight = normal·view). Recovers photo-crisp detail on the front. Needs the
  predictor-space camera convention calibrated once (render a known mesh,
  compare projections; TripoSR's training camera is not exported with the
  ONNX graphs).
- **Higher default MC resolution for final exports** (256 → 320/384) now that
  smoothing+reprojection hide grid artifacts; scale texel density with it.

### 3. Rejected / parked (license or feasibility)

| Model | Verdict |
|---|---|
| TRELLIS / TRELLIS.2-4B (MS, MIT) | Best open quality but 4B CPU-hostile, sparse attention has no clean ONNX path, and the texture stage depends on **nvdiffrast (NVIDIA non-commercial)**. Park until someone strips the NC deps (Hi3DGen proved it's possible for geometry). |
| Hunyuan3D-2.x (Tencent) | Community license **excludes EU/UK/South Korea** + MAU cap → fails redistribution bar. |
| SPAR3D / SF3D (Stability) | Community license revenue cap → rejected (same as #764 spike). |
| InstantMesh (Apache) | Pipeline requires Zero123++ weights (**CC-BY-NC**) → rejected. |
| MeshAnything, CraftsMan | NC / AGPL-tainted weights → rejected. |
| Fine-tune TripoSG on CC-subset Objaverse (~740K permissive objects) | Feasible (PartCrafter fine-tuned the DiT only) but weeks of GPU work; only worth it for domain specialization later. |

## What Meshy/Tripo actually do differently
Multi-stage native-3D pipelines: big geometry diffusion (quad topology, part
segmentation) + a separate multi-view **PBR texture diffusion** stage
(albedo/normal/roughness/metallic) + retopo/UV post-stages. Open models match
the geometry of roughly one commercial generation back (TripoSG ≈ Tripo 2.0);
the durable gaps are texture/PBR quality and quad retopo — and the project
already has PBRify map synthesis (#404) and QuadRetopo (#401) to chain onto
generated meshes (`qtmesh material --texture gen_diffuse.png --generate-pbr`,
`qtmesh retopo`).
