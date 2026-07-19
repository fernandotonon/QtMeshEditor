# Third-party AI models

QtMeshEditor downloads optional ML model files on first use (never bundled in
the binary). Attribution + licenses for the models and their training data:

> **Hosting layout:** the app downloads everything from the aggregate
> [`fernandotonon/QtMeshEditor-models`](https://huggingface.co/fernandotonon/QtMeshEditor-models)
> HF repo. Each converted model also has a **dedicated mirror repo** with the
> full standalone model card, per-model license, and I/O contract —
> [`QtMeshEditor-unirig-onnx`](https://huggingface.co/fernandotonon/QtMeshEditor-unirig-onnx),
> [`QtMeshEditor-skintokens-onnx`](https://huggingface.co/fernandotonon/QtMeshEditor-skintokens-onnx),
> [`QtMeshEditor-triposr-onnx`](https://huggingface.co/fernandotonon/QtMeshEditor-triposr-onnx),
> [`QtMeshEditor-triposg-onnx`](https://huggingface.co/fernandotonon/QtMeshEditor-triposg-onnx),
> [`QtMeshEditor-pbrify-onnx`](https://huggingface.co/fernandotonon/QtMeshEditor-pbrify-onnx),
> [`QtMeshEditor-realesrgan-onnx`](https://huggingface.co/fernandotonon/QtMeshEditor-realesrgan-onnx),
> [`QtMeshEditor-u2net-onnx`](https://huggingface.co/fernandotonon/QtMeshEditor-u2net-onnx),
> [`QtMeshEditor-smolvlm-gguf`](https://huggingface.co/fernandotonon/QtMeshEditor-smolvlm-gguf),
> and the five performance-capture (#869) MediaPipe graphs
> [`QtMeshEditor-blazeface-onnx`](https://huggingface.co/fernandotonon/QtMeshEditor-blazeface-onnx),
> [`QtMeshEditor-facemesh-onnx`](https://huggingface.co/fernandotonon/QtMeshEditor-facemesh-onnx),
> [`QtMeshEditor-faceblendshapes-onnx`](https://huggingface.co/fernandotonon/QtMeshEditor-faceblendshapes-onnx),
> [`QtMeshEditor-blazepose-onnx`](https://huggingface.co/fernandotonon/QtMeshEditor-blazepose-onnx),
> [`QtMeshEditor-poselandmarks-onnx`](https://huggingface.co/fernandotonon/QtMeshEditor-poselandmarks-onnx)
> (plus the in-house
> [`QtMeshEditor-rmib-inbetween`](https://huggingface.co/fernandotonon/QtMeshEditor-rmib-inbetween),
> [`QtMeshEditor-mesh-segmentation`](https://huggingface.co/fernandotonon/QtMeshEditor-mesh-segmentation),
> [`QtMeshEditor-t2m`](https://huggingface.co/fernandotonon/QtMeshEditor-t2m)).
> Mirrors are refreshed with `scripts/sync-hf-model-repos.sh`.

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

### UniRig skinning head (issue #819 Slice C) — decision record

- **Status: not exported.** UniRig's second stage (skin-weight prediction,
  the Bone-Point Cross Attention head) runs its mesh geometry through a
  **PTv3 (Point Transformer V3) backbone built on spconv sparse
  convolutions**, plus flash-attn MHA modules (verified against
  `src/model/unirig_skin.py` upstream, 2026-07). spconv ops have **no ONNX
  operator lowering** — the skeleton-stage export only worked because that
  stage never executes the PTv3/spconv path (it was stubbed out). A faithful
  `skin.onnx` therefore requires re-implementing the PTv3 forward densely, a
  research task, not an export chore.
- **SkinTokens / TokenRig evaluated as the issue's decision gate asks**
  (https://github.com/VAST-AI-Research/SkinTokens): code **MIT**; predicts
  the full rig (skeleton + skinning) as one token sequence via an FSQ-CVAE
  weight tokenizer + a Qwen3-0.6B autoregressive transformer — **no spconv**,
  so it is ONNX-exportable with the same KV-cache decoder pattern as the
  #408 skeleton export. It *is* the preferred ML-skinning path, but it is a
  full new integration (its own weight tokenizer + AR decode + a different
  skeleton representation), not a drop-in head on our predicted skeletons.
  Weights-license confirmation on its HF release is part of that follow-up.
- **What ships today:** `SkinWeights::Algorithm::UniRigML` exists on every
  surface (GUI/CLI/MCP) and falls back to GeodesicVoxel with a clear
  `fallbackReason` — the fallback the issue specifies. The ML path is
  implemented via SkinTokens (next section).

## SkinTokens / TokenRig — ML skin-weight prediction (issue #819 Slice C)

- **Model:** SkinTokens ("a learned, compact, discrete representation for
  skinning weights") + TokenRig, the unified autoregressive rig transformer
  built on it. We run it SKELETON-TEACHER-FORCED: the existing skeleton
  (ours, #407's, or #408's) is tokenized as the prefix and only the skin
  tokens are generated.
- **Source:** VAST-AI-Research — https://github.com/VAST-AI-Research/SkinTokens
  — code **MIT**. Weights: https://huggingface.co/VAST-AI/SkinTokens —
  **MIT** (license tag verified 2026-07, ungated). The AR backbone is
  Qwen3-0.6B (**Apache-2.0**).
- **Components** (exported by `scripts/export-skintokens-onnx.py`, a
  one-time offline developer tool — not shipped): `mesh_cond.onnx`
  (Michelangelo shape encoder + projection), `vae_cond.onnx` (FSQ-CVAE
  conditioning encoder), `embed.onnx` + `decoder.onnx` (Qwen3-0.6B causal
  step with explicit KV cache), `skin_decode.onnx` (FSQ code lookup + the
  CVAE weight decoder), plus `skintokens.json` (the config manifest the
  C++ runtime reads — vocab layout, tokens-per-skin, point count,
  normalisation). Export parity vs the torch reference validated to ~1e-5
  relative on every graph.
- The app runs the graphs via ONNX Runtime (`src/SkinTokensPredictor.cpp`),
  downloading them on first use to `AppData/ai_models/skintokens/`
  (~2.3 GB — the decoder alone is 1.66 GB fp32). Base URL override
  `QTMESH_SKINTOKENS_MODEL_BASE_URL` / `QSettings ai/skintokensModelBaseUrl`;
  offline guard `QTMESH_SKINTOKENS_NO_DOWNLOAD`. Any failure (non-ONNX
  build, models absent, invalid hierarchy) falls back to GeodesicVoxel with
  a reported reason.

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

## Mesh part segmentation (issue #410, categories #818 B2)

- **Models:** a family of PointNet++-style point-cloud part-segmentation
  networks, one small ONNX per mesh CATEGORY, plus a tiny category classifier
  for Auto dispatch — all exported to ONNX and run by `src/MeshSegmenter.cpp`
  (the fourth ONNX consumer):
  - `meshseg.onnx` — body (head / torso / left+right arm / left+right leg);
  - `meshseg_vegetation.onnx` — trunk / branch / foliage / root / flower;
  - `meshseg_vehicle.onnx` — vehicle_body / wheel / window / wing / rotor;
  - `meshseg_building.onnx` — wall / roof / window / door / chimney / foundation;
  - `meshseg_category.onnx` — point-cloud → {body, vegetation, vehicle,
    building} (PointNet max-pool classifier, ~0.1 MB).
  The non-body models are trained on **procedurally generated synthetic
  shapes we own** (parametric trees / vehicles / buildings with exact
  by-construction labels — CC0, ours); the classifier trains on the same
  generators + the mined body corpus. No third-party data at all in those
  four files.
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
- **Hosting:** every `meshseg*.onnx` (body + the category models + the
  classifier) is hosted in the
  [`fernandotonon/QtMeshEditor-models`](https://huggingface.co/fernandotonon/QtMeshEditor-models)
  HF repo under `segment/` (the app's download source) and downloads on first
  use to `AppData/ai_models/segment/` (one shared override
  `QTMESH_SEGMENT_MODEL_BASE_URL` / `QSettings ai/segmentModelBaseUrl`;
  offline guard `QTMESH_SEGMENT_NO_DOWNLOAD` covers the whole family).
  Each model also has a dedicated standalone HF repo with its own model card
  (`QtMeshEditor-mesh-segmentation` for body, plus
  `…-mesh-segmentation-{vegetation,vehicle,building,category}`), refreshed
  from the aggregate via `scripts/sync-hf-model-repos.sh`.
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

## ICT-FaceKit — ARKit blendshape template for face auto-rig (epic #889)

- **Asset (not a learned model):** the ICT-FaceKit generic neutral head
  (`generic_neutral_mesh.obj`) + its per-expression meshes named after the
  ARKit blendshapes (`jawOpen`, `mouthSmile_L`, `eyeBlink_L`, `browInnerUp_L`,
  …), all sharing one topology (26,719 verts) so each shape = expr − neutral.
- **Source / license:** [USC-ICT/ICT-FaceKit](https://github.com/USC-ICT/ICT-FaceKit)
  — **MIT** (Copyright 2020 USC Institute for Creative Technologies). The
  standard/released model is MIT; a separate "full model" tier under a
  USC-specific license is **REJECTED** (we ship only the MIT tier). MIT clears
  the permissive-redistribution bar, so the template + shapes are hostable on
  the `fernandotonon/QtMeshEditor-models` HF repo (Slice B, #890) — packed by
  `scripts/export-arkit-template.py` into `facerig/arkit_template.bin` and
  uploaded by `scripts/upload-facerig-template.sh`; it downloads on first use.
- **How it is used:** the template is the *source* for **deformation transfer**
  (Sumner & Popović 2004) — QtMeshEditor fits it to the user's neutral head via
  native non-rigid ICP (Amberg 2007), then transfers each of the 52 ARKit
  expressions onto the user's topology, attaching them as `Ogre::Pose` morph
  targets so face performance capture (#869) works on the mesh. **No ML model,
  no ONNX** — it is a deterministic geometry algorithm (sparse linear solve),
  implemented natively in `src/FaceRig/` (Slices C/D/E). The offline spike
  (`scripts/spike-facerig.py`, not shipped) validated the approach first
  (see `docs/FACE_RIG_SPIKE.md`). Verified end-to-end on
  a decimated, different-topology face: mean 0.008% / max 0.61% NRICP fit and
  51 attached shapes. Surfaced via `qtmesh facerig`, MCP `add_arkit_blendshapes`,
  and the Inspector "Add ARKit Blendshapes" button. See `docs/FACE_RIG.md`.
- **Facial-landmark anchoring (landmark pass):** the NRICP fit is anchored to
  real face features by **MediaPipe Face Mesh V2** (`face_landmarks.onnx`,
  **Apache-2.0** — the same model the mocap face-capture uses, #869). We render
  the head front-on, detect the 478 landmarks, back-project them to the mesh
  surface, and pin the matching template vertices — so the template lands on the
  actual eyes/nose/mouth instead of a low-residual-but-mis-oriented drape. Hosted
  under `facerig/face_landmarks.onnx` (a copy of the mocap graph); downloads on
  first use; `ENABLE_ONNX`-guarded with a graceful unanchored-fit fallback.
- **Rejected alternatives:** Wrap3D (commercial, used by the reference impl for
  NRICP — we implement NRICP natively instead), FLAME-based 3DMMs
  (research-only), any generative expression model on non-commercial data.
  Landmark detectors trained on 300W / WFLW / InsightFace (dlib, PIPNet,
  2d106det) were rejected — their weights carry research-only / non-commercial
  terms; MediaPipe FaceMesh (Apache-2.0) is the clean choice.

## Performance capture (epic #869)

### MediaPipe Face Landmarker + Pose Landmarker — face/pose capture (#870/#872/#874)

- **Models:** Google MediaPipe `face_landmarker.task` (BlazeFace short-range
  detector + Face Mesh V2 478-landmark model + 52-blendshape MLP-Mixer) and
  `pose_landmarker_full.task` (BlazePose detector + 39-landmark model with
  world coordinates), converted TFLite → ONNX.
- **License:** **Apache-2.0, code AND models** (Google's MediaPipe release —
  the stack the entire VTuber ecosystem builds on). Ship the Apache-2.0
  notice next to the hosted weights.
- The ONNX export is produced by `scripts/export-facecap-onnx.py` (one-time,
  offline dev tool — not shipped), which also **asserts numerical parity**
  against the Python `mediapipe` reference (landmarks ≤ 0.59 px, blendshapes
  ≤ 0.0148 abs, pose world landmarks ≤ 1.02 cm on the test set). Conversion
  recipe, pre/post-processing contracts and measured latencies:
  `docs/MOCAP_SPIKE.md`. The app runs the five graphs via ONNX Runtime,
  downloading them on first use to `AppData/ai_models/mocap/{face,pose}/`.
- **Rejected face alternatives:** DECA / EMOCA / SPECTRE (all regress the
  FLAME 3DMM — research-only license), ARKit (iOS-only), OpenSeeFace (MIT
  code but weaker blendshape story).

### SAM 3D Body + MHR — body capture quality path (#870/#874) — decision record

- **MHR (Momentum Human Rig)** — the 127-joint parametric rig SAM 3D Body
  poses — is **Apache-2.0** (https://github.com/facebookresearch/MHR, assets
  v1.0.1). Skeleton definition (names, hierarchy, pre-rotations, derived rest
  world pose) extracted to `mhr_skeleton.json` by
  `scripts/export-bodycap-onnx.py --mhr-assets`. Apache-2.0 permits use,
  modification, and redistribution (including our derived `mhr_skeleton.json`)
  for any purpose incl. commercial, provided the LICENSE + NOTICE are retained
  and modifications are marked — we ship the Apache-2.0 notice next to the
  derived skeleton on the HF repo. No copyleft, no field-of-use restriction.
- **SAM 3D Body checkpoints** (`facebook/sam-3d-body-dinov3`, 2.1 GB) are
  under the **SAM License** (2025-11-19 text reviewed in full, 2026-07-12).
  **Verdict: PASS with conditions** — usable as an OPTIONAL,
  downloaded-on-first-use backend, never bundled:
  - §1.a grants use/reproduction/distribution/modification (ONNX conversion
    is a permitted modification, not the prohibited "reverse engineering" of
    §1.b.iv). §1.b.i permits redistributing derivatives **only under the SAM
    License with a copy attached** — rehosting converted ONNX on our HF repo
    is compliant with the license file shipped next to the weights.
  - **No non-commercial clause; model outputs are unrestricted** (§3 only
    disclaims warranty over outputs). No EU exclusion (unlike Hunyuan3D).
  - Conditions/risks recorded: the SAM backend is NOT permissive-equivalent —
    users of that one optional feature are bound by the SAM License
    (AUP-style trade-controls/military restrictions pass through); Meta may
    unilaterally amend the terms (§8); upstream access is gated (HF
    click-through sharing contact info with Meta).
  - **Status:** checkpoint download blocked pending the gated-access
    acceptance on the HF model page (our token is not yet on the authorized
    list). Export recipe proven by the community port
    (AmmarkoV/SAM3DBody-cpp: DINOv3-H+ backbone ~4.8 GB fp32 + 93 MB decoder
    → 519 MHR params). Until unblocked the body path ships **MediaPipe Pose +
    analytic IK only** (Apache-2.0, zero conditions).
- **Rejected body alternatives:** WHAM / GVHMR / TRAM / 4D-Humans (all regress
  **SMPL** — weights non-commercial, Meshcapade sells the commercial
  license), OpenPose (CMU non-commercial), FreeMoCap (AGPL).

All of the above clear QtMeshEditor's permissive-redistribution bar (MIT app,
distributed via Homebrew / WinGet / Snap / Docker). GPL/CC-BY-NC/unlicensed
models are deliberately excluded (e.g. RigNet was rejected for #408 — GPL code +
unlicensed weights — in favour of UniRig; ShapeNet-Part/PartNet were rejected
for #410 — non-commercial — in favour of synthetic bone-weight-derived labels).
