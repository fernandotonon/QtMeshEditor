# Performance capture — Spike Findings & Contracts (#869 / slice A #870)

**Epic:** [#869 — AI: Performance capture: video/webcam → facial morph + skeletal body animation](https://github.com/fernandotonon/QtMeshEditor/issues/869)
**Slice:** [#870 — Spike: model conversion + licensing due diligence](https://github.com/fernandotonon/QtMeshEditor/issues/870) (de-risk first)
**Status:** Spike — **GO** for the face path and the MediaPipe-Pose-IK body
fallback (both risks fully retired, parity proven). SAM 3D Body: **license
passes with conditions**, export recipe proven by a community port, but the
checkpoint download is **gated** behind a per-account HF click-through we have
not completed yet — Slice E starts with Pose-IK and lights the SAM backend up
when access is granted (the UniRig "plumbing first, hosting later" precedent).

---

## TL;DR

- **MediaPipe → ONNX conversion — DONE, parity proven.** All five TFLite
  models (face detector / face landmarks / face blendshapes / pose detector /
  pose landmarks) convert with `tf2onnx` and load under ONNX Runtime.
  `scripts/export-facecap-onnx.py` converts AND asserts parity against the
  Python `mediapipe` reference in one run:
  - face landmarks: worst **0.59 px** (target ≤ 2 px), typical 0.01–0.12 px mean
  - blendshapes: worst **0.0148** abs (target ≤ 0.02), typical ≤ 0.003 mean
  - pose world landmarks: worst **1.02 cm** (target ≤ 2 cm)
- **Latency (M-series CPU, ONNX Runtime CPU EP, detector every frame):**
  face full pipeline **11.2 ms/frame (~89 fps)**, pose full **17.2 ms (~58 fps)**.
  Live-mode ≥15 fps target clears with 5× headroom before detector-skip.
- **Model sizes:** face 6.4 MB total (0.4 + 4.9 + 1.1), pose 24.7 MB
  (11.9 + 12.8). Trivial hosting/download.
- **Licenses:** all five MediaPipe models Apache-2.0 (code AND weights).
  MHR rig definition Apache-2.0. SAM 3D Body weights under the **SAM License**
  — verdict + conditions in `THIRD_PARTY_AI_MODELS.md`.
- **MHR skeleton — DONE.** 127-joint definition (names, hierarchy,
  translation offsets, pre-rotations, derived rest world rotations/positions)
  extracted from the Apache-2.0 `mhr_model.pt` by
  `scripts/export-bodycap-onnx.py --mhr-assets` → `mhr_skeleton.json`.

---

## Conversion recipe (pinned versions)

Python 3.12 venv (arm64): `mediapipe 0.10.35`, `tensorflow 2.21.0`,
`tf2onnx 1.17.0`, `onnx 1.22.0`, `onnxruntime 1.27.0`, opencv (mediapipe dep).

```bash
# .task bundles are zips of TFLite models (Apache-2.0):
# https://storage.googleapis.com/mediapipe-models/face_landmarker/face_landmarker/float16/1/face_landmarker.task
# https://storage.googleapis.com/mediapipe-models/pose_landmarker/pose_landmarker_full/float16/1/pose_landmarker_full.task
python scripts/export-facecap-onnx.py \
    --face-task .mocap_work/models/face_landmarker.task \
    --pose-task .mocap_work/models/pose_landmarker_full.task \
    --images .mocap_work/images --out-dir .mocap_work/out
```

Three conversion gotchas, all handled inside the script:

1. **`pose_detector.tflite` has fp16 block-sparse weights behind DENSIFY ops**
   which tf2onnx cannot parse (`ValueError: cannot reshape array...`). Fix:
   run the TFLite interpreter with `experimental_preserve_all_tensors=True`
   **and `BUILTIN_WITHOUT_DEFAULT_DELEGATES`** (XNNPACK folds DENSIFY away and
   `get_tensor()` then reads garbage — this cost an hour), capture each
   DENSIFY output, rewrite the flatbuffer dense, drop the DENSIFY ops.
2. **`face_blendshapes.tflite` (MLP-Mixer) converts but tf2onnx's optimizer
   emits a graph ORT rejects** (transpose optimizer breaks a LayerNorm Mul →
   `ShapeInferenceError`). Fix: convert that model with optimizers disabled
   (auto-fallback in `convert_with_fallback`).
3. **Resampling fidelity is the parity maker-or-breaker**: PIL's antialiased
   BILINEAR gave 2.5 px / 0.31-blendshape errors on a rotated face; switching
   letterbox + rotated-crop to cv2 `warpAffine` (plain bilinear, exact float
   matrices, BORDER_ZERO — what MediaPipe itself does) collapsed errors to
   0.01 px. **Slice C must use non-antialiased bilinear in C++.**

## The pre/post-processing contract (what Slice C implements)

Machine-readable copy: `mocap_contract.json` (written next to the converted
models). Landmark/blendshape name tables live in the script constants.

### Face (three sessions)

1. **Detector** (BlazeFace short-range): letterbox (keep aspect, centre, zero
   border) to **128×128**, RGB `/127.5 - 1` → `regressors [1,896,16]`,
   `classificators [1,896,1]`. SSD anchors: strides `[8,16,16,16]`,
   min/max scale 0.1484375/0.75, offset 0.5, fixed size, interpolated aspect
   1.0 → 896 anchors (each `[cx,cy]`). Decode: `xy = raw/128 + anchor`,
   `wh = raw/128`; 6 keypoints (right eye, left eye, nose, mouth, right ear,
   left ear) same decode; score `sigmoid(clip(raw,-100,100))`, threshold 0.5,
   **weighted** NMS at IoU 0.3. Un-letterbox all coords.
2. **ROI**: rotation `θ = normalize(-atan2(-(eyeL.y-eyeR.y), eyeL.x-eyeR.x))`
   (target angle 0, y-down image coords, R(θ)=[[c,-s],[s,c]]); rect = detection
   box centre, side `max(w,h)·1.5` (square_long → scale). Crop: sample the
   rotated rect to **256×256**, RGB `/255` (range [0,1]), zero border:
   `p_img = centre + R(θ)·((u/256-0.5)·w, (v/256-0.5)·h)`.
3. **Landmarks** (Face Mesh V2): outputs `[N,1,1,1434]` = **478 landmarks ×
   (x,y,z) in 256-crop pixels** (no attention-refinement assembly needed — V2
   is single-tensor), `[N,1,1,1]` face-presence **logit** (apply sigmoid),
   `[N,1]` unused aux. Project back: divide by 256, then
   `x' = cx + dx·w·cosθ - dy·h·sinθ` (same R(θ)); `z' = z·w`.
4. **Blendshapes**: gather the 146-landmark subset (indices in the script /
   contract JSON), feed **pixel coords** `(x·imgW, y·imgH)` as `[1,146,2]` →
   `[52]` scores in [0,1]. Order = MediaPipe's canonical 52 (ARKit-compatible,
   `_neutral` first) — the `FaceCap::kBlendshapeNames` vocabulary.
5. **Head pose**: weighted Kabsch/Umeyama fit of the 468 canonical face model
   vertices (`canonical_face_model.obj`, cm) onto screen landmarks
   `(x·W, -y·H, -z·W)` using MediaPipe's 33 Procrustes basis weights
   (`geometry_pipeline_metadata_landmarks.pbtxt`); keep rotation (+translation),
   solve-and-discard scale. Convention: +X right, +Y up, camera looks -Z;
   identity = facing the camera. This intentionally replaces MediaPipe's
   perspective geometry pipeline — measured delta vs its facial transformation
   matrix is 4–9.3° on the test images (fine for driving a Head bone; the
   deltas are systematic, not jitter, and the recorder calibrates the first
   confident frame as neutral anyway).

**Detector-skip strategy (live mode):** re-run the detector only when the
landmark presence drops below ~0.5; otherwise derive the next ROI from the
previous frame's landmarks (bounding box of the 478 points, same 1.5×
square-long expansion, rotation from eye corners 33→263 — MediaPipe's own
tracking mode).

### Pose (two sessions)

1. **Detector** (BlazePose): letterbox to **224×224**, `/127.5 - 1` →
   `[1,2254,12]` + `[1,2254,1]`. Anchors: strides `[8,16,32,32,32]`, same
   scale params → 2254. 4 keypoints; decode identical to face (scale 224).
2. **ROI**: centre = keypoint 0 (mid-hip), radius = |kp1 − kp0|,
   box = square of side `2·radius·1.25`, rotation
   `θ = normalize(π/2 − atan2(-(dy), dx))` (target angle 90°).
3. **Landmarks**: crop 256×256, `/255` → `[1,195]` = **39 × (x,y,z,
   visibility-logit, presence-logit)** in 256-crop pixels (apply sigmoid to
   vis/presence; landmarks 33..38 are auxiliary — drop), `[1,1]` pose-presence
   (**already a probability** — do NOT sigmoid, unlike the face flag),
   `[1,256,256,1]` segmentation (ignore), `[1,64,64,39]` heatmap (ignore),
   `[1,117]` = **39 × (x,y,z) world landmarks in metres, hip-centred**.
   Screen landmarks project like face; world landmarks are rotated by the ROI
   rotation ONLY (no scale/translate).
4. Landmark index → body part table: 0 nose, 1-6 eyes, 7/8 ears, 9/10 mouth,
   11/12 shoulders, 13/14 elbows, 15/16 wrists, 17-22 fingers, 23/24 hips,
   25/26 knees, 27/28 ankles, 29/30 heels, 31/32 foot index (L/R = odd/even).

## Parity numbers (2026-07-12, the script asserts these on every run)

| image | landmarks mean/max (px) | blendshapes mean/max | head-pose Δ |
|---|---|---|---|
| portrait.jpg | 0.01 / 0.03 | 0.0002 / 0.0009 | 5.1° |
| portrait_rotated.jpg | 0.01 / 0.08 | 0.0004 / 0.0016 | 9.3° |
| portrait_small.jpg | 0.02 / 0.09 | 0.0011 / 0.0080 | 5.4° |
| face_stylizer_raw_face_demo.png | 0.12 / 0.59 | 0.0026 / 0.0148 | 4.0° |
| pose.jpg (world landmarks) | 0.61 cm mean / 1.02 cm max | — | — |

Test images are MediaPipe's own Apache-2.0 test assets
(`storage.googleapis.com/mediapipe-assets`).

## SAM 3D Body (the body quality path)

- **License verdict — PASS with conditions** (full analysis in
  `THIRD_PARTY_AI_MODELS.md`): the SAM License permits modification (ONNX
  conversion) and redistribution of derivatives **only under the SAM License
  with a copy attached** — rehosting converted weights on our HF repo is
  compliant if the license file ships next to them. No non-commercial clause;
  outputs unrestricted. Conditions to carry: license text alongside weights +
  surfaced in-app, AUP-style trade-controls restrictions pass through to
  users, Meta may unilaterally amend (§8).
- **Access — BLOCKED on a human click-through**: the HF repo
  (`facebook/sam-3d-body-dinov3`, 2.1 GB `model.ckpt`) is gated; our token
  reads repo metadata but file downloads return "not in the authorized list".
  Action item (Slice E): accept the SAM License on the model page with the
  project's HF account, then run the export.
- **Export feasibility — proven by a community port**:
  [SAM3DBody-cpp / Fast-SAM-3D-Body](https://github.com/AmmarkoV/SAM3DBody-cpp)
  already runs the model under ONNX Runtime as `backbone.onnx` (DINOv3-H+,
  ~4.8 GB fp32, image → [1280,32,32] features) + `decoder.onnx` (~93 MB,
  → [B,1024] token → FFN heads → **519 MHR params: global 6D rotation,
  per-joint Euler XYZ, shape, hands, face**). ~150–200 ms/frame on an RTX
  3090 ⇒ seconds/frame on M-series CPU — offline video only, never live.
  fp16 (~2.4 GB) is the realistic hosting tier (SkinTokens 2.3 GB precedent).
- **MHR (Momentum Human Rig) — Apache-2.0, extracted**: 127 joints
  (73 core + twist/null helpers), parent-before-child ordered,
  `(x,y,z,w)` pre-rotation quats, translation offsets in cm, rest pose ≈
  A-pose standing at Y-up ~185 cm. `scripts/export-bodycap-onnx.py
  --mhr-assets` writes `mhr_skeleton.json` with derived
  `restWorldRotation`/`restWorldPosition` per joint — the `W · clip · W⁻¹`
  conjugation input Slice E's retarget needs (the `cmuRestWorld` analogue).
  Momentum rest semantics: `worldRot(j) = worldRot(parent)·preRot(j)`,
  `worldPos(j) = worldPos(parent) + worldRot(parent)·offset(j)`; animated
  local rotation composes AFTER the pre-rotation.

## Go/No-Go

- **Face path: GO.** Conversion + parity + latency all retired; Slice C is a
  wiring job against the contract above.
- **Pose-IK body fallback: GO.** Models converted with the same parity bar;
  world landmarks are the IK solver's input (Slice E).
- **SAM 3D Body backend: GO-when-unblocked.** License compatible (with the
  redistribution conditions recorded), recipe proven externally, skeleton
  data in hand. Blocked solely on the gated-access acceptance; Slice E ships
  Pose-IK first regardless (the epic's degrade-don't-die plan).
