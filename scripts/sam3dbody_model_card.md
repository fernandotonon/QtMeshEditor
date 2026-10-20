---
license: other
license_name: sam-license-and-apache-2.0
license_link: https://huggingface.co/fernandotonon/QtMeshEditor-sam3dbody-onnx/blob/main/LICENSE-SAM.txt
library_name: onnx
pipeline_tag: image-to-3d
tags:
  - onnx
  - human-body
  - mesh-recovery
  - sam-3d-body
  - mhr
  - qtmesheditor
base_model:
  - facebook/sam-3d-body-dinov3
---

# SAM 3D Body (body branch) + MHR — ONNX, for QtMeshEditor

ONNX export of **Meta's SAM 3D Body** (`facebook/sam-3d-body-dinov3`, body-only inference path) and of the
**MHR** (Momentum Human Rig) body model, as used by [QtMeshEditor](https://github.com/fernandotonon/QtMeshEditor)
for `qtmesh body photo.jpg -o body.obj` — a human body mesh (18,439 vertices) from ONE photo.

This repository is the standalone model card + mirror. The application downloads from the aggregate repo
`fernandotonon/QtMeshEditor-models` under `sam3dbody/`; both hold identical files.

## Files

| file | what | licence |
|---|---|---|
| `sam3d_backbone.onnx` + `.onnx.data` | DINOv3-ViT-H+/16 backbone, fp32, image `[1,3,512,512]` (RGB 0..1) → features `[1,1280,32,32]` | SAM License |
| `sam3d_decoder.onnx` | camera-ray conditioning + promptable decoder (6 layers, intermediate pose feedback) + pose/camera heads | SAM License |
| `mhr_body.onnx` | MHR body forward (shape `[1,45]` + model params `[1,204]` → vertices in cm, joint translations/rotations), no pose correctives, no face model | Apache-2.0 (MHR data) |
| `mhr_faces.i32` | MHR body topology, 36,874 triangles, int32 little-endian | Apache-2.0 (MHR data) |
| `LICENSE-SAM.txt`, `LICENSE-MHR.txt` | the licence texts — **the SAM License requires a copy to travel with the SAM materials** | |

## Provenance and fidelity

Exported by `scripts/export-sam3dbody-onnx.py` in the QtMeshEditor repository from Meta's checkpoint (the script
imports Meta's own package from a local clone; no Meta source files are redistributed). Every graph was checked
against Meta's PyTorch run on the same inputs: decoder outputs agree to ~1e-6, backbone to ~2e-5 (fp32 here vs bf16
in Meta's code), MHR to ~1e-4 cm. What is deliberately **not** reproduced: MHR pose correctives (a 664 MB network;
a posed body moves by a mean 2 mm / max 25 mm without it) and the face-expression model (the body head zeroes it).

Pre-processing (person crop, affine warp, CLIFF condition, camera rays) lives in the application
(`src/BodyFromPhoto/SamBodyPredictor.cpp`), not in the graphs.

## Licence — read before using

* `sam3d_backbone.onnx*` and `sam3d_decoder.onnx` are derivative works of Meta's **SAM Materials** and are
  distributed under the **SAM License** (`LICENSE-SAM.txt`; Meta Platforms, Last Updated November 19, 2025).
  It is not an OSI open-source licence: it carries an indemnity clause, a litigation-termination clause, trade-control
  restrictions and Meta's right to update the terms. By using these files you accept it.
* `mhr_body.onnx` and `mhr_faces.i32` derive from Meta's **MHR** release data, which is **Apache-2.0**
  (`LICENSE-MHR.txt`).
* Hosting here follows the maintainer's own reading of the SAM License and is not legal advice.

Not affiliated with or endorsed by Meta.
