---
license: apache-2.0
library_name: onnx
pipeline_tag: mask-generation
tags:
  - onnx
  - segment-anything
  - sam2
  - mesh-segmentation
  - qtmesheditor
base_model:
  - facebook/sam2.1-hiera-small
---

# SAM 2.1 (Hiera-Small) — ONNX export, image encoder + prompt decoder

**ONNX re-export of Meta's [SAM 2.1 Hiera-Small](https://github.com/facebookresearch/sam2)**
(*"SAM 2: Segment Anything in Images and Videos"*, Ravi et al. 2024 — Apache-2.0
code + Apache-2.0 weights). All credit for the model goes to Meta AI / FAIR.

Two graphs for **single-image** automatic mask generation (no video memory):
used by **[QtMeshEditor](https://github.com/fernandotonon/QtMeshEditor)**'s
generic part segmentation (#1179). It renders a mesh from many views, runs these
graphs on each render, and lifts the masks back onto the mesh. That is our own
C++ implementation of the SAMesh method, with local inference through ONNX
Runtime and no Python at runtime.

> The files QtMeshEditor downloads at runtime live in the shared
> [`fernandotonon/QtMeshEditor-models`](https://huggingface.co/fernandotonon/QtMeshEditor-models)
> repo under `segment/`. This repo is the standalone model card + mirror.

## Files

| file | role | size |
|---|---|---|
| `sam2.1_hiera_small_encoder.onnx` | image encoder: `image[1,3,1024,1024]` (RGB, ImageNet mean/std normalised) → `image_embed[1,256,64,64]`, `high_res_feat0[1,32,256,256]`, `high_res_feat1[1,64,128,128]` (`no_mem_embed` already added) | 138 MB |
| `sam2.1_hiera_small_decoder.onnx` | prompt encoder + mask decoder: the three encoder outputs + `point_coords[B,N,2]` (pixels of the 1024 input frame) + `point_labels[B,N]` (1 = foreground, 0 = background, -1 = padding) → `mask_logits[B,3,256,256]` + `iou_scores[B,3]` (multimask output) | 21 MB |
| `LICENSE-SAM2.txt` | Apache-2.0 (Meta SAM 2) | |

`B` (prompts per run) and `N` (points per prompt) are dynamic. Upsample the
256² logits to the image size bilinearly (align_corners = false) and threshold
at 0.

## How it was exported

`scripts/export-sam2-onnx.py` in the QtMeshEditor repo (TorchScript exporter,
opset 17) from the official `sam2.1_hiera_small.pt` checkpoint, SAM 2 commit
`2b90b9f5`. The padding-point embedding is rewritten functionally (the
exporter otherwise emits `Add` nodes with empty inputs that only fail at ONNX
Runtime load time). The script refuses to finish unless:

- both graphs load in ONNX Runtime and produce finite outputs;
- on an 8×8 point grid over three mesh renders, every mask matches Meta's
  `SAM2ImagePredictor` with IoU ≥ 0.99 (measured ≥ 0.9995) and every
  predicted IoU score within 1e-3;
- the dynamic `B` / `N` shapes work.

## Automatic mask generation (what QtMeshEditor does around it)

The reimplemented part of `SAM2AutomaticMaskGenerator`: a 16×16 prompt grid
restricted to the object's pixels, batches of 64 single-point prompts, keep
masks with predicted IoU ≥ 0.7 and stability ≥ 0.85 (logit offset 1.0), mask
NMS at IoU 0.7, drop masks under 0.1 % of the object. About 45–85 s per mesh
for 40 renders on an Apple M-series CPU.

## Licence

Apache-2.0, as the original SAM 2 code and checkpoints. `LICENSE-SAM2.txt` is
Meta's licence text. These graphs are a format conversion of Meta's weights;
no fine-tuning.
