---
license: other
license_name: mit-dinov3-apache-2.0
license_link: https://huggingface.co/fernandotonon/QtMeshEditor-triposplat-onnx#licence--read-before-using
library_name: onnx
pipeline_tag: image-to-3d
tags:
  - onnx
  - image-to-3d
  - 3d-gaussians
  - gaussian-splatting
  - triposplat
  - qtmesheditor
base_model:
  - VAST-AI/TripoSplat
---

# TripoSplat — ONNX export (staged graphs)

**ONNX re-export of [VAST-AI/TripoSplat](https://huggingface.co/VAST-AI/TripoSplat)**
(*"Generative 3D Gaussians with Learned Density Control"*, arXiv 2605.16355 —
MIT code + MIT weights): one image → up to **262,144 3D Gaussians**. All credit
for the original weights goes to VAST-AI-Research.

Exported as **five staged graphs** so the Euler+CFG flow loop, the octree
systematic sampling, and the splat→mesh conversion run in host code — for
**[QtMeshEditor](https://github.com/fernandotonon/QtMeshEditor)**
(`qtmesh generate3d --backend triposplat`, GUI Backend dropdown, MCP `backend`
arg), local inference via ONNX Runtime. No Python at runtime.

> The files QtMeshEditor downloads at runtime live in the shared
> [`fernandotonon/QtMeshEditor-models`](https://huggingface.co/fernandotonon/QtMeshEditor-models)
> repo under `triposplat/`. This repo is the standalone model card + mirror.

## Files

| file | role | licence |
|---|---|---|
| `triposplat_dinov3.onnx` + `.onnx.data` | DINOv3 ViT-H image encoder: `image[1,3,1024,1024]` in `[0,1]` → `feature1[1,4101,1280]` (ImageNet norm + un-affine layer_norm baked in) | DINOv3 License |
| `triposplat_vae.onnx` + `.onnx.data` | FLUX.2 VAE encoder: `image` + `noise[1,32,128,128]` → `feature2[1,4101,128]` | Apache-2.0 (FLUX.2 AE) |
| `triposplat_flow.onnx` + `.onnx.data` | one DiT flow step: `latent[B,8192,16]`, `camera[B,1,5]`, `t[B]` (×1000), features → `v_latent`, `v_camera` | MIT |
| `triposplat_octree.onnx` + `.onnx.data` | octree density logits: parent cell centres + level + latent → `logits[1,L,8]` | MIT |
| `triposplat_gs.onnx` + `.onnx.data` | Gaussian decoder: leaf points + latent → xyz / SH-DC / opacity / scale / rotation (32 Gaussians per point) | MIT |
| `LICENSE-TripoSplat.txt` | MIT (VAST-AI TripoSplat) | |
| `LICENSE-DINOv3.txt` | DINOv3 License — **§1.b.i requires a copy with any redistribution of DINOv3 weights or derivatives** | |
| `LICENSE-Apache-2.0.txt` | Apache-2.0 for the FLUX.2 VAE encoder | |

~6.1 GB total (fp32). Every graph ships a small `.onnx` proto next to an
`.onnx.data` external-weights file; both must sit side by side or ONNX Runtime
cannot load the graph.

## Inference contract

- **Matte first.** Composite the subject over **black** (the training
  convention). QtMeshEditor mattes with its own U²-Net / BiRefNet path, then
  erodes alpha 1 px, square-crops 1.2× the alpha box, and resizes to 1024².
- **Flow loop (host code):** shifted schedule `t = 3ℓ/(1+2ℓ)`, Euler steps,
  classifier-free guidance with **zero features** as the unconditional branch.
  Default 20 steps / guidance 3; **10 steps / guidance 3 is reliable** (halves
  wall time). Guidance must stay > 1.
- **Octree:** 8 levels of coarse-to-fine point placement; logits split the
  remaining point budget over 8 children by systematic sampling, then a
  uniform jitter inside the leaf cell.
- **Gaussians → mesh** is *not* in these graphs — QtMeshEditor densifies the
  splats on a 1024³ grid (`SplatToMesh`), simplifies, UV-unwraps, and bakes
  colour + a detail normal. The raw splats are also written as a standard
  3DGS `.ply`.
- Attention is exported as `com.microsoft.MultiHeadAttention` (plain SDPA
  materialises a ~9.7 GB score matrix per block under ORT). CoreML cannot
  compile the flow graph; macOS runs on the CPU EP.

Measured parity vs the numpy+ORT replay: 262,144 splats within 3e-6 position /
3e-4 opacity. Full notes: [`docs/TRIPOSPLAT.md`](https://github.com/fernandotonon/QtMeshEditor/blob/master/docs/TRIPOSPLAT.md).

## Reproducing

`scripts/export-triposplat-onnx.py` in the QtMeshEditor repo (one-time,
offline; needs a local TripoSplat checkout + the HF checkpoints). Export one
graph per process (`--only`) on a ≤24 GB machine.

## Licence — read before using

Three licence texts ship next to the graphs:

| file | covers |
|---|---|
| [`LICENSE-TripoSplat.txt`](LICENSE-TripoSplat.txt) | MIT — TripoSplat code + most converted weights (VAST-AI-Research) |
| [`LICENSE-DINOv3.txt`](LICENSE-DINOv3.txt) | Meta **DINOv3 License** for `triposplat_dinov3.onnx*` (commercial OK; **not** MIT — do not call the stack "entirely MIT") |
| [`LICENSE-Apache-2.0.txt`](LICENSE-Apache-2.0.txt) | Apache-2.0 for the FLUX.2 VAE encoder inside `triposplat_vae.onnx*` (Black Forest Labs released the AE under Apache-2.0; the FLUX.2 [dev] *model* is non-commercial and is **not** used) |

Not affiliated with or endorsed by VAST-AI-Research, Meta, or Black Forest Labs.
