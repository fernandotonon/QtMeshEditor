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

All of the above clear QtMeshEditor's permissive-redistribution bar (MIT app,
distributed via Homebrew / WinGet / Snap / Docker). GPL/CC-BY-NC/unlicensed
models are deliberately excluded (e.g. RigNet was rejected for #408 — GPL code +
unlicensed weights — in favour of UniRig).
