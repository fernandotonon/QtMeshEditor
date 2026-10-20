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
