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
  RigNet / #409 LAFAN1). The shipped v2 model is trained on a mix of
  **synthetic surface-sampled bodies we own** (three parametric body plans —
  humanoid incl. chibi proportions, quadruped, biped-with-tail — with exact
  by-construction labels; CC0, ours) and **CC0 rigged characters mined for
  exact rig-derived labels** (Quaternius packs; provenance ledger kept with
  the corpus). The derivation + labels are ours, all sources are CC0, so the
  weights are redistributable. See `docs/MESH_SEGMENTATION_STRATEGY.md` for
  the v1 failure analysis, the canonicalisation pipeline, and the
  multi-category roadmap.
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
- **Model architecture:** a PointNet++-style segmenter with two kNN
  local-aggregation blocks (~1 MB ONNX), trained at the app's inference
  sample size (4096 points). Validated against EXACT rig-derived ground truth
  on held-out CC0 rigs and on out-of-distribution rigged characters kept out
  of training; accuracy figures live in `docs/MESH_SEGMENTATION_STRATEGY.md`.
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
- **Data:** a curated set of clips from the **CMU Graphics Lab Motion Capture
  Database** (mocap.cs.cmu.edu) — **permissively licensed, commercial-OK** (the same
  source as the #409 RMIB model). Built offline by `scripts/build-motion-library.py`
  into a `qtmesh-motion-library-v3` JSON (per-frame, per-joint canonical WORLD-frame
  quaternions on the 22-joint CMU core-body skeleton) — no model weights, no AMASS.
  The v4 library build selects each clip's **active window** (max motion energy,
  snapped to a calm near-neutral start frame — the retarget deltas against clip
  frame 0), replacing the first-4-seconds slices that mostly captured idle
  lead-ins, and covers **13 actions** (walk, run, jump, dance, march, kick,
  punch, wave, climb, sit, throw, boxing, idle, sweep, wash) with SEVERAL
  takes per action — the matcher picks among them at random so repeat
  generates vary while every result is real mocap.
- **Hosting:** `motion/motion-library.json` (~4 MB, 47 clips / 15 actions) in the
  [`fernandotonon/QtMeshEditor-models`](https://huggingface.co/fernandotonon/QtMeshEditor-models)
  HF repo, downloaded on first use to `AppData/ai_models/motion/` (override
  `QTMESH_MOTION_LIBRARY_BASE_URL` / `QSettings ai/motionLibraryBaseUrl`; offline
  guard `QTMESH_MOTION_NO_DOWNLOAD`).
- **Retargeting:** `AnimationMerger::applyMotionClip` maps the canonical clip onto
  the user's rig via `MotionInbetween::canonicalIndexForBone` (shared with #409).
- **Generative path (opt-in, experimental) — trained by us.** `motion/t2m.onnx`
  (+ `t2m-vocab.json`), a CVAE transformer trained **from scratch on the same
  CMU source** by `scripts/prep-t2m-v4.py` + `scripts/train-t2m-onnx-v4.py`
  (offline dev tools, not shipped): 30 fps world-frame windows with neutral
  starts, absolute-pose decoder, per-sample + rotation-space velocity matching,
  derived-local supervision (what the retarget renders), and z=0 latent
  supervision (the app's inference condition). The vocab json declares
  `"frame":"world"` so model clips ride the same retarget path as the template
  library. Selected via `--model` (CLI) / `model:true` (MCP) / the GUI
  checkbox; the template library remains the default and the automatic
  fallback. Same CMU licensing basis as above.

All of the above clear QtMeshEditor's permissive-redistribution bar (MIT app,
distributed via Homebrew / WinGet / Snap / Docker). GPL/CC-BY-NC/unlicensed
models are deliberately excluded (e.g. RigNet was rejected for #408 — GPL code +
unlicensed weights — in favour of UniRig; ShapeNet-Part/PartNet were rejected
for #410 — non-commercial — in favour of synthetic bone-weight-derived labels).
