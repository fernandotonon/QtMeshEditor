> **Note:** the Fit action / `qtmesh dna fit` this document refers to has been removed (poor results); a predicted MHR body is now an OBJ you can use directly.

# Image-based body prediction (#1100) — licence check and spike

OpenRigLogic is the rig layer; it reconstructs nothing from a photo. A photo → body step needs a human mesh recovery
(HMR) model whose output the existing Fit action (`docs/DNA_FITTING.md`) can adapt the MetaHuman body to. Checked
2026-10-19 against the upstream repositories and release assets themselves, not mirrors.

## Candidates

| | code | weights / body model | commercial use | gating / runtime |
|---|---|---|---|---|
| **SAM 3D Body** (Meta, CVPR 2026) | SAM License (`LICENSE`, 8 KB, same text in both HF repos) | `model.ckpt` + `assets/mhr_model.pt`, **SAM License** | not prohibited and not expressly granted (see below) | HF repos `facebook/sam-3d-body-vith` / `-dinov3`, **manual approval** (`gated=manual`; the 403 was reproduced); Python 3.11, detectron2 at a pinned commit; GPU/CPU/MPS support not stated |
| **MHR** (Meta Momentum Human Rig — the body model SAM 3D Body outputs) | Apache-2.0 | `assets.zip` of release v1.0.1 carries a `LICENSE.txt` that is **plain Apache-2.0** (11,358 bytes, read) | yes | public, no gating; TorchScript model (LOD 1 only), 7 LODs of FBX with a 205-bone rig, 45 identity + 204 pose + 72 expression parameters |
| **Anny** (NAVER Labs) | Apache-2.0 | body model Apache-2.0 | yes | a body model, not a photo predictor; pretrained HMR weights and the Anny-One dataset were not checked |
| SMPL / SMPL-X | research | research | no (restricted) | ruled out |

### The SAM License, read in full
Non-exclusive, worldwide, royalty-free, **non-transferable**; use, reproduce, distribute, modify, derive. It does not
mention commercial use either way (the only limits are in §1.b). Restrictions: no ITAR / Trade-Controls end uses
(military, warfare, nuclear, espionage, weapons), no reverse engineering. Unusual terms worth a decision before
shipping the weights: Meta **may modify the agreement at any time, effective immediately** (continued use = acceptance);
the licensee **indemnifies Meta** against third-party claims (§5.b); licences end on a patent/IP suit; outputs are "as is"
with no ownership statement; the text has drafting slips ("SAM 3 Materials", undefined "Work"). Research publications
must acknowledge the materials.

**Consequence.** Unlike every other model in this project (MIT / Apache / BSD weights, hosted on our HF repo), SAM 3D Body's
weights cannot be mirrored and downloaded transparently: access is per-account manual approval, and redistributing
them carries the licence and the indemnity. The honest options are (a) an opt-in download where the USER accepts Meta's
terms on their own Hugging Face account (the editor never redistributes), or (b) wait for a permissively licensed
predictor. **MHR itself is clean** (Apache-2.0 code and assets), so using MHR as a *target body* needs no new licence.

## Spike

What could be run without the gated weights: take an MHR body as the "predicted" mesh and push it through the existing
pipeline (`lod1.fbx` from the public MHR release, mean identity, 205 bones).

* The joint-name table had no MHR names, so `qtmesh dna fit --register-joints` paired 2 joints and refused. MHR's
  `c_head / c_neck / c_spine0,1,3 / l_uparm / l_lowarm / l_wrist / l_upleg / l_lowleg / l_foot` (+ `r_`) are now in
  `defaultJointNameMap`; **all 17 anchors match**. (Pelvis = MHR `root`: spine tilt 2.8°, versus 6.6° with `c_spine0`.)
* Result: facing, shoulders and hips aligned to 0.0°, spine tilt 2.8°, joints 22 mm from the target's bones on average;
  the surface wrap follows the target's proportions. The MetaHuman body is a FEMALE template, so a male MHR target keeps some
  of its chest shape (the smooth wrap does not reach a different gender's anatomy — same limit as fitting Rumba).

Not done (blocked on access to the checkpoints): running SAM 3D Body on a photo. To continue, request access on both HF
repos with your account (`huggingface-cli login` is already set up here), then: `demo.py --image_folder … --output_folder …`
→ the MHR mesh/parameters → export a mesh through the MHR TorchScript model → `qtmesh dna fit … --target <that mesh>`.
Open questions for that step: whether inference runs on Apple MPS/CPU in usable time (not stated upstream), and a
Python 3.11 arm64 environment (the default `python3` here is an x86_64 build whose torch cannot load).

## Shipped: the ONNX port (2026-10-20)

`qtmesh body <photo> -o body.obj [--pose neutral|posed] [--bbox x1,y1,x2,y2] [--facing-z] [--json]` and MCP
`generate_body_from_photo`. Code: `src/BodyFromPhoto/SamBodyPredictor.{h,cpp}` (+ `BodyFromPhotoCLI`); export:
`scripts/export-sam3dbody-onnx.py`; hosting: `scripts/upload-sam3dbody-models.sh` (card: `scripts/sam3dbody_model_card.md`);
licences/decision record: `THIRD_PARTY_AI_MODELS.md`.

Pipeline: person box (explicit, or the alpha matte of the existing U²-Net background remover) → padded 1.25, fixed to aspect 0.75 then
1:1 → 512² affine warp (bilinear, zero border like `cv2.warpAffine`) → backbone → decoder (CLIFF condition + camera rays,
the reference's own formulas, pinned by a unit test against values captured from Meta's pipeline) → MHR forward twice (posed;
neutral = pose params zeroed, shape + scale kept). A real photo takes ~32 s end to end on an Apple-silicon CPU (backbone
~2 s of that is the graph itself; the rest is loading 3.4 GB of weights).

Facts learned getting it through the exporter (each is also in the export script's docstring):
* the anti-aliased ray downsample must become two matmuls, and the weights must be extracted with a spatial dim of 8 (a dim of 1
  takes a different PyTorch resample path and produced weights that looked right and were off by 0.9);
* `roma`'s euler conversion and several `x[idx] = y` / `x[..., [1, 2]] *= -1` lines in Meta's heads export as `Add` nodes with
  EMPTY inputs: the export succeeds and only onnxruntime's loader rejects the file, so verify by loading, not by exporting;
* the dinov3 backbone type skips the `x * 16/12` keypoint-grid scaling the ViT variants apply (leaving it in broke parity by 0.3).

Measured against Meta's PyTorch on identical inputs: decoder ~1e-6, backbone ~2e-5, MHR ~1e-4 cm; the full C++ run's shape
parameters are within 0.015 of Meta's body-only reference (the remainder is bf16 vs fp32 and resampling).

### Next
* Use the MHR joints (the `mhr_body.onnx` graph already returns 127 joint translations/rotations) as the target skeleton for
  `--register-joints --refine-surface`; the neutral OBJ alone is the surface-only route.
* GUI action (Object mode) and an undoable "fit to this photo" that chains `generate_body_from_photo` → `fit_dna_body`.
* Pose correctives for the posed output (664 MB; not needed for the neutral body the fit uses).

## Animation from SAM 3D Body

`qtmesh mocap <video|--frames-dir DIR> --body --mesh rig.fbx [--algo sam3dbody|pose-ik]`. Per frame: person box (matting) -> backbone -> decoder -> `mhr_body.onnx` (127 joint rotations and positions). The clip is then retargeted through the project's incompatible-skeleton retargeter with the MHR rest pose as the source rest (see CLAUDE.md for the mechanics and the measurements).

**Fidelity** (`scripts/anim-bone-direction-error.py`, 12 rendered frames of a Mixamo dance, bone-direction error vs the original clip): pose-ik 27 deg; first SAM version (22 CMU roles, deltas against frame 0) 20 deg; current 11.3 deg (spine 5.6, legs 5.9, hands 16, arms 18).

What the measurements showed, in order of impact: SAM's pose is camera-relative (a camera looking down read as a 24 deg forward lean; its joint directions were within 10 deg of truth after one global rotation), the first-frame calibration of the CMU path discards the starting pose, and rest-direction alignment must be limited to limbs (it bends the spine by the difference between two rigs' rest curvature).

### Hand refinement (Meta's "full" inference)

The body pass predicts the fingers from the 512 px body crop, where a hand is a few dozen pixels. Meta's full pipeline
(`SAM3DBody.run_inference(..., inference_type="full")`) refines both hands, and the editor now does the same
(`SamBodyHands.{h,cpp}` for the geometry, `Runner::Impl::refineWithHands` for the flow):

1. the body decoder also predicts two **hand boxes** (hand-box tokens -> `bbox_embed`);
2. each hand is cropped (padding 0.9) and run through the backbone + a dedicated **hand decoder**; the LEFT hand runs on
   the mirrored photo (the hand decoder only knows right hands) and is un-mirrored afterwards;
3. a hand is accepted when four tests pass: its wrist within 1.4 rad of the body's, crop > 64 px, every hand keypoint inside
   its crop, its wrist within a quarter box of the body wrist (Meta divides each by the OTHER hand's box; kept for parity);
4. the body decoder is re-run with the accepted wrists + the elbows as **keypoint prompts**;
5. the hand poses, hand scales and wrist rotations are dropped into the body (`sam3d_hand_fuse.onnx`).

Graphs (`scripts/export-sam3dbody-onnx.py --hands`): `sam3d_decoder_v2.onnx` (body decoder with prompts, hand boxes and
`pose_raw` for the re-run), `sam3d_hand_decoder.onnx`, `sam3d_hand_fuse.onnx`. The prompt tokens sit at the END of the token
list (attention is permutation-equivariant; verified) and there are always 4 of them, the unused ones masked out of the
self-attention with Meta's own x_mask rule: **with a dynamic prompt count ONNX Runtime 1.31 took ~100 s to load the graph**
(1.32: 20 s), with the fixed count 17 s.

Parity: each graph vs Meta's module 1e-6..2e-5. End to end on Meta's `dancing.jpg` (vertices within 12 cm of each wrist,
reference = Meta full): Meta body-only 32.6 / 27.2 mm mean (left / right), ours body-only 32.0 / 28.1, **ours refined
1.9 / 4.1 mm** (whole body 5.2 mm, the rest is mostly the skipped pose correctives). Cost: ~45 s of one-time session loading
per process, then ~4 s per frame (2 backbone runs). Hands under 64 px are skipped (as in Meta). Off with `refineHands=false` /
`--no-hand-refine` / MCP `refine_hands:false` / the panel checkbox. The graphs are optional: without them the body-only
estimate is used and the status line says so. `QTMESH_SAM_TIMING=1` prints per-stage timings.

### Root motion

The decoder's camera translation (`cam_t`, camera frame) gives where the body is relative to the camera. For a static camera
its change is the person's travel: `rootTrajectory` maps it into the MHR frame (Y and Z negated), relative to the first frame,
with a centred moving average (1 frame for x/y, 4 for the depth, which comes from the person's apparent size and is the noisy
axis), and the retargeter's Root mode moves the target's hips by it (scaled by the rigs' height ratio). On by default for video
and camera takes, off for pose photos and with `--in-place`. A moving camera adds its own motion.

### Next
* **Arms (18 deg):** clavicle/shoulder mapping and SAM's per-arm accuracy; try the MHR twist joints for the forearm roll.
* **One person box per sequence** (matting each frame costs ~1 s), batching, CoreML/GPU provider.
* **MCP / GUI:** `capture_body_from_video` and the live capture panel still use pose-ik.
* **Real footage:** the evaluation above is rendered frames; run a real video and judge by eye.
* **Corpus use:** MHR joints from video as a cleaner source for the motion library than MediaPipe landmarks.
