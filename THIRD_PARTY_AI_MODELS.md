# Third-party AI models

QtMeshEditor downloads optional ML model files on first use (never bundled in
the binary). Attribution + licenses for the models and their training data:

## UniRig — auto-rig skeleton prediction (issue #408)

- **Model:** UniRig skeleton-prediction (autoregressive transformer + Michelangelo
  shape encoder), exported to ONNX.
- **Source:** VAST-AI-Research / Tsinghua — *"One Model to Rig Them All: Diverse
  Skeleton Rigging with UniRig"*, SIGGRAPH 2025.
  https://github.com/VAST-AI-Research/UniRig — code **MIT**.
  Weights: https://huggingface.co/VAST-AI/UniRig — **MIT**.
- **Training data:** Articulation-XL2.0
  (https://huggingface.co/datasets/Seed3D/Articulation-XL2.0) — **CC-BY-4.0**.
  Attribution: Seed3D / the Articulation-XL2.0 authors.
- The ONNX export is produced by `scripts/export-unirig-onnx.py` (a one-time,
  offline developer tool — not shipped). The app runs the resulting
  `encoder.onnx` + `decoder.onnx` via ONNX Runtime (`src/UniRigPredictor.cpp`),
  downloading them on first use to `AppData/ai_models/unirig/`.

## TripoSR — image-to-3D mesh generation (epic #764)

- **Model:** TripoSR single-image 3D reconstruction (DINO ViT tokenizer +
  triplane transformer + NeRF decoder), exported to ONNX as an encoder
  (image → triplane) + decoder (triplane + points → density/color) pair.
- **Source:** Tripo AI + Stability AI — *"TripoSR: Fast 3D Object Reconstruction
  from a Single Image"* (arXiv 2403.02151).
  https://github.com/VAST-AI-Research/TripoSR — code **MIT**.
  Weights: https://huggingface.co/stabilityai/TripoSR — **MIT** (code AND weights).
- MIT code+weights is the deciding factor: it clears QtMeshEditor's permissive-
  redistribution bar (Homebrew / Snap / WinGet / Docker), the same reason UniRig
  (#408) passed. Non-commercial SF3D / Stable-Fast-3D was rejected on license.
- The host-side iso-surface step (density grid → mesh) is a native, from-scratch
  Lorensen marching cubes (`src/ImageTo3D/MarchingCubes.{h,cpp}`, public-domain
  tables — no vendored/GPL dependency); TripoSR's own `torchmcubes` is torch/GPU-only.
- The ONNX export is produced by `scripts/export-triposr-onnx.py` (one-time,
  offline developer tool — not shipped). The app runs the resulting encoder +
  `triposr_decoder.onnx` via ONNX Runtime (`src/ImageTo3D/MeshGenPredictor.cpp`),
  downloading them on first use to `AppData/ai_models/triposr/`.
- **Encoder size tiers** (all the SAME MIT weights, just re-precisioned by the
  export script — no separate license): `triposr_encoder.onnx` (fp32, ~1.68 GB) and
  `triposr_encoder_int8.onnx` (~430 MB, ORT dynamic quantization). The user picks
  the tier; each downloads on demand. (fp16 was dropped — TripoSR's attention has a
  hardcoded Cast-to-float32 the ONNX fp16 converters can't rewrite into a loadable
  graph; int8 is smaller anyway.)
- **Hosted** on the `fernandotonon/QtMeshEditor-models` HF repo:
  `triposr/triposr_encoder.onnx`, `triposr/triposr_encoder_int8.onnx`,
  `triposr/triposr_decoder.onnx`, `rembg/u2net.onnx` (uploaded via
  `scripts/upload-triposr-models.sh`). First use downloads them; if ever absent the
  feature reports a clean "not yet hosted" state (no crash) — the RigNet precedent.

## U²-Net — background removal for image-to-3D (epic #764)

- **Model:** U²-Net salient-object detection (`u2net.onnx`) — the default
  foreground-segmentation model shipped by [rembg](https://github.com/danielgatis/rembg).
- **Source:** Qin et al., *"U²-Net: Going Deeper with Nested U-Structure for
  Salient Object Detection"* (Pattern Recognition 2020),
  https://github.com/xuebinqin/U-2-Net — code **Apache-2.0**; the released ONNX
  weights are redistributed by rembg under the same permissive terms.
- Used only as a **pre-process** for TripoSR image-to-3D (`src/BackgroundRemover.cpp`):
  isolate the subject so the reconstruction sees a clean background. Downloads on
  first use to `AppData/ai_models/rembg/u2net.onnx` (override
  `QTMESH_REMBG_MODEL_BASE_URL` / `QSettings ai/rembgModelBaseUrl`; offline guard
  `QTMESH_REMBG_NO_DOWNLOAD`). Falls back to the raw image when unavailable.

## PBRify_Remix — PBR map synthesis (issue #404)

- Three SPAN models from https://github.com/Kim2091/PBRify_Remix — **CC0-1.0**,
  trained on CC0 AmbientCG / Poly Haven textures.

## Real-ESRGAN — texture upscaling (issue #405)

- Real-ESRGAN x2plus / x4plus from https://github.com/xinntao/Real-ESRGAN —
  **BSD-3-Clause**.

## RMIB — animation in-betweening (issue #409)

- **Model:** an RMIB-style (Robust Motion In-betweening) transformer that
  predicts intermediate poses between two keyframes, exported to ONNX (~13 MB).
- **Algorithm/paper:** Harvey, Yurick, Nowrouzezahrai, Pal — *"Robust Motion
  In-betweening"*, SIGGRAPH 2020 (Ubisoft La Forge). The *algorithm* (a
  transition transformer over a fixed skeleton/feature layout) is published and
  unencumbered; the app ships a from-scratch ONNX runtime for it
  (`src/MotionInbetween.cpp`), not Ubisoft's research code — and the shipped
  weights are **our own**, trained from scratch (see below), NOT Ubisoft's.
- **Training data:** **CMU Graphics Lab Motion Capture Database**
  (mocap.cs.cmu.edu) — permissively licensed: free to use/modify/redistribute
  *including in commercial products*; the only restriction is you may not RESELL
  the motion data itself. Credit: mocap.cs.cmu.edu. This is what makes our
  weights redistributable under the project's permissive bar — the rest of the
  in-betweening field standardizes on **LAFAN1** (Ubisoft LaForge), which is
  **CC-BY-NC-ND** (non-commercial / no-derivatives) and was therefore rejected,
  same posture as RigNet for #408.
- **Skeleton:** trained on the 22 CMU core-body joints (hips/spine/neck/head +
  both arms + both legs). At runtime `MotionInbetween::canonicalIndexForBone()`
  maps arbitrary rig bones (Mixamo / generic / CMU naming) onto these 22 roles;
  rigs that don't resolve a strong majority fall back to the spline.
- **Export tool:** `scripts/export-rmib-onnx.py` (one-time, offline, NOT shipped
  — the app never runs Python). Produces `rmib.onnx` (input `[1,2,220]` →
  output `[1,8,220]`).
- **Hosting:** `rmib.onnx` is hosted in the
  [`fernandotonon/QtMeshEditor-models`](https://huggingface.co/fernandotonon/QtMeshEditor-models)
  HF repo under `inbetween/` and downloads on first use to
  `AppData/ai_models/inbetween/` (override `QTMESH_INBETWEEN_MODEL_BASE_URL` /
  `QSettings ai/inbetweenModelBaseUrl`; offline guard `QTMESH_INBETWEEN_NO_DOWNLOAD`).
- **Fallback:** when ONNX is disabled, the model can't be fetched, or a rig
  doesn't map to the canonical skeleton, the feature uses its deterministic
  spline fallback (cubic-Hermite + shortest-arc slerp) — always present, needs
  no model, and visibly smoother than naive linear interpolation. The trained
  model measurably beats slerp on held-out CMU motion (rotation error < half).

## Mesh part segmentation (issue #410)

- **Model:** a PointNet++-style point-cloud part-segmentation network (per-point
  → head / torso / left+right arm / left+right leg), exported to ONNX. Run by
  `src/MeshSegmenter.cpp`; the fourth ONNX consumer.
- **Training data — synthetic / permissively derived.** The standard
  part-segmentation datasets (**ShapeNet-Part**, **PartNet**) are
  **non-commercial research-only** and so were rejected (same bar as #408
  RigNet / #409 LAFAN1). Instead the shipped model is trained on **synthetic
  data we own**: per-vertex part labels derived from **rigged-humanoid bone
  weights** (each vertex's dominant bone → a canonical body part) on permissively
  -licensed source rigs (e.g. CMU-derived), sampled into point clouds. The
  derivation + labels are ours (CC0), so the weights are redistributable.
- **Mined real data (continual improvement) — CC0 / CC-BY only.** Synthetic
  primitives can't capture real surface distributions, so the corpus is
  optionally augmented with REAL rigged characters mined for exact labels via
  `qtmesh segment <mesh> --dump-training-data` (rig bone-weight → bone name →
  part; `AutoRig::rigPriorPartLabels`). **Because the shipped weights are a
  derived work of the training data, only sources that clear the project's
  permissive-redistribution bar may be mined:** CC0 (Quaternius / Poly Pizza /
  Kenney rigged characters) or CC-BY with attribution recorded in the corpus
  `SOURCES.md`. **Mixamo (Adobe) is explicitly NOT used** — its EULA does not
  clearly permit training a redistributable commercial model, and "trained on"
  obligations are an unsettled legal frontier; shipping Mixamo-derived weights
  would be the first asset in the repo resting on "probably fine" rather than
  "provably clean", inconsistent with every other AI asset here. `scripts/
  fetch-training-rigs.sh` (curated CC0 source ledger) + `scripts/
  mine-training-data.sh` assemble + mine the corpus; `export-meshseg-onnx.py
  --real-data <dir>` mixes it into the synthetic set.
- **Model architecture:** a small PointNet-style segmenter (~0.3 MB ONNX).
  Validated: 98% per-point accuracy on held-out synthetic humanoids, and it
  transfers to real humanoid meshes (labels a Mixamo character into symmetric
  head/torso/arms/legs — better limb symmetry than the geometric fallback).
- **Export tool:** `scripts/export-meshseg-onnx.py` (one-time, offline, NOT
  shipped — the app never runs Python; it synthesises the data + trains + exports).
- **Hosting:** `meshseg.onnx` is hosted in the
  [`fernandotonon/QtMeshEditor-models`](https://huggingface.co/fernandotonon/QtMeshEditor-models)
  HF repo under `segment/` and downloads on first use to
  `AppData/ai_models/segment/` (override `QTMESH_SEGMENT_MODEL_BASE_URL` /
  `QSettings ai/segmentModelBaseUrl`; offline guard `QTMESH_SEGMENT_NO_DOWNLOAD`).
- **Fallback:** when ONNX is disabled, the model can't be fetched, or inference
  fails, the feature uses a deterministic **geometric** segmenter (connected-
  component islands + an up-axis/lateral spatial heuristic, refined by rig
  bone-proximity when available) — always present, no model needed, and good
  enough for reasonable head/torso/limb labels on upright humanoids.

## Text-to-motion template library (issue #411)

- **Not a learned model — a curated motion CLIP LIBRARY.** The #411 spike (see
  `docs/TEXT_TO_MOTION_SPIKE_411.md`) found that off-the-shelf generative text-to-
  motion models (MDM, T2M-GPT, MotionGPT) are all trained on **HumanML3D / KIT-ML**,
  which derive from **AMASS** = **academic / non-commercial** (the same wall as
  #409 LAFAN1). A from-scratch generative model proved feasible-but-hard (collapses
  without multi-day ML effort), so the shipped MVP is a **template-clip** approach.
- **Data:** a small curated set of clips from the **CMU Graphics Lab Motion Capture
  Database** (mocap.cs.cmu.edu) — **permissively licensed, commercial-OK** (the same
  source as the #409 RMIB model). Built offline by `scripts/build-motion-library.py`
  into a `qtmesh-motion-library-v1` JSON (per-frame, per-joint canonical-quaternion
  poses on the 22-joint CMU core-body skeleton) — no model weights, no AMASS.
- **Hosting:** `motion/motion-library.json` (~0.9 MB, 10 actions) in the
  [`fernandotonon/QtMeshEditor-models`](https://huggingface.co/fernandotonon/QtMeshEditor-models)
  HF repo, downloaded on first use to `AppData/ai_models/motion/` (override
  `QTMESH_MOTION_LIBRARY_BASE_URL` / `QSettings ai/motionLibraryBaseUrl`; offline
  guard `QTMESH_MOTION_NO_DOWNLOAD`).
- **Retargeting:** `AnimationMerger::applyMotionClip` maps the canonical clip onto
  the user's rig via `MotionInbetween::canonicalIndexForBone` (shared with #409).
- **Generative path:** postponed; the dev prototype `scripts/export-t2m-onnx.py`
  is kept for a future upgrade but is NOT shipped (no model file).

All of the above clear QtMeshEditor's permissive-redistribution bar (MIT app,
distributed via Homebrew / WinGet / Snap / Docker). GPL/CC-BY-NC/unlicensed
models are deliberately excluded (e.g. RigNet was rejected for #408 — GPL code +
unlicensed weights — in favour of UniRig; ShapeNet-Part/PartNet were rejected
for #410 — non-commercial — in favour of synthetic bone-weight-derived labels).
