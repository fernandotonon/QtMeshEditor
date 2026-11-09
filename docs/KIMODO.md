# Text to motion with NVIDIA Kimodo

**Built with Meta Llama 3.**

QtMeshEditor generates skeletal animation from text with NVIDIA **Kimodo** (`Kimodo-SOMA-RP-v1.2`), then retargets
it onto your rig. It runs **natively**: the text encoder through the bundled llama.cpp and the motion model through
ONNX Runtime, with no Python. An optional Python sidecar (`ai/kimodo/`) remains as an alternative runtime.

## First use: download the models (~9.2 GB, once)

Nothing downloads until you ask:

- **Panel**: Animation mode → Animations → *Download Kimodo models*.
- **CLI**: add `--download` to a Kimodo command.
- **MCP**: pass `download: true` to `generate_motion`.

Files land in `<AppData>/ai_models/kimodo/` and are checked against pinned SHA-256 digests. A Cancel / Stop
interrupts the transfer. Overrides: `QTMESH_KIMODO_MODEL_BASE_URL` / `QTMESH_KIMODO_TEXTENC_BASE_URL` (or QSettings
`ai/kimodoModelBaseUrl` / `ai/kimodoTextEncoderBaseUrl`), `QTMESH_KIMODO_NO_DOWNLOAD`.

## Panel

Select a rigged character, switch to **Animation** mode, and in **Animations** type a prompt under
*generate from text* (Model: NVIDIA Kimodo). Defaults: 2 s, 1 variation, root motion off (the clip stays in place).
One prompt per line makes a continuous multi-step motion; `@SECONDS` at the end of a line sets that step's length:

```
walk forward @3
sit down @2
```

Every variation becomes its own clip (`generated_kimodo`, `generated_kimodo_v2`, …), added in one undo step.

## Loops

Tick **Loop** (CLI `--loop`, MCP `loop: true`) to get a clip that repeats seamlessly. It needs the native runtime
and one prompt; *Seconds* (0.5–10) is the loop length you want, and the cycle found sets the real one (a 2 s walk is
two strides). How it is made (`src/AIMotion/KimodoLoop.h`):

1. Kimodo generates once. The two frames about one cycle apart whose poses match best are found, comparing a window
   of 5 frames so the speed matches too, skipping the first 0.5 s (Kimodo starts every clip from rest) and any
   stretch that barely moves (an attack's final hold would otherwise loop as a frozen pose).
2. When they match within 3° (most walks, runs and idles), that cycle is cut out and its last frames are cross-faded
   into the frames before its start. One generation.
3. Otherwise Kimodo generates again with the cycle's first 5 frames pinned at the start AND at the end (the end
   moved forward by one stride), so the seam lands on the exact pose. A motion with no cycle at all (over 12°, e.g.
   an attack) pins its own starting pose at both ends instead, so it returns to where it began.

The last frame of every loop equals the first plus the stride, so an engine looping the clip neither skips nor
repeats a frame. `--loop-mode cut|regenerate` (MCP `loop_mode`) forces one method; the panel status, the CLI and
the MCP report (`kimodo.loops`) say which was used, the loop length and how well the cycle matched. Combine with
root motion off for in-place loops.

## CLI

```bash
qtmesh anim rigged.glb --generate "a person jumps forward" --backend kimodo --duration 2 -o out.glb
qtmesh anim rigged.glb --backend kimodo --segment "walk forward@3" --segment "sit down@2" -o out.glb
qtmesh anim rigged.glb --generate "wave" --backend kimodo --variations 3 --seed 7 --no-root-motion -o out.glb
qtmesh anim rigged.glb --apply-motion motion_00.npz -o out.glb   # re-apply a saved motion, any rig, no model
```

Options: `--runtime auto|native|python` (auto = native when its models are downloaded, else the sidecar),
`--download`, `--variations 1..16`, `--seed N`, `--diffusion-steps 1..1000` (default 100), `--no-root-motion`,
`--no-postprocess`, `--loop`, `--loop-mode auto|cut|regenerate`, `--name CLIP`, `--json`. Kimodo-only options are refused without `--backend kimodo`.
`--constraints` (Kimodo constraint files) needs the Python runtime.

## MCP

```json
{"name": "generate_motion", "arguments": {"backend": "kimodo", "prompt": "a person jumps forward", "duration": 2,
  "variations": 1, "root_motion": false, "download": true, "output_path": "/tmp/jump.glb"}}
```

Also `segments`, `seed`, `diffusion_steps`, `postprocess`, `runtime`, `loop`, `loop_mode`, `clip_name`, and `motion_files` (re-apply saved
motions). Options are validated before any download; Stop interrupts a download or a generation; a failed export
takes the new clips back out of the scene.

## Speed and accuracy

On an Apple M-series Mac, ~18 s per 5 s variation at the default 100 steps, plus ~4 s to load the models and encode the prompt (fewer `diffusion_steps` is faster). The
native runtime reproduces Kimodo's own PyTorch output on the same inputs to a 2e-4 maximum joint-rotation
difference and 0.003 cm root difference, with identical foot contacts (`scripts/export-kimodo-onnx.py --verify`).

## QtMesh T2M: the small, experimental alternative

The same panel has a **Model** picker: *NVIDIA Kimodo (default)* or *QtMesh T2M (experimental, small)*, QtMeshEditor's
own text-to-motion model (trained on Kimodo-generated clips and our CC0/CC-BY motion library). It is meant for lower-end machines:

| On this Mac (M-series, one 3 s clip) | NVIDIA Kimodo | QtMesh T2M |
|---|---|---|
| Download | ~9.2 GB | ~740 MB |
| Text encoding | ~1.1 s (+ loading the 8 GB encoder) | ~0.3 s |
| Motion sampling | ~14 s | ~2.7 s |
| Whole CLI run (import, generate, export) | ~18-21 s | ~5 s |
| Peak resident memory | ~7.6-8.2 GB | ~0.87 GB |

Expect lower quality: one prompt per clip (no `@SECONDS` timelines), up to 5 s, and results vary with the seed
(of ten jump seeds, four bend the knees 70-86 degrees and four barely move: generate a few variations). The model
outputs rotations only; with **Root motion** on (`--no-root-motion` / `root_motion: false` turn it off) the editor
moves the character by the slide of the planted foot (measured: walk ~1.4 m/s, run ~1.9 m/s); the root never
rises, so jumps stay on the spot. It is a separate download (*Download QtMesh T2M* in the
panel, `--download` / `download: true`).

```bash
qtmesh anim rigged.glb --generate "a person waves with the right hand" --backend qtmesh_t2m --duration 3 --download -o out.glb
```

MCP: `{"backend": "qtmesh_t2m", "prompt": "...", "duration": 3, "variations": 1, "seed": 1, "download": true}`.
Clips are named `generated_qtmesh_t2m`, `_v2`, …, in one undo step. They are retargeted with the same
incompatible-skeleton retargeter as Kimodo (#523), so a rig facing −Z gets the motion turned, not mirrored.

## Licences

- Kimodo weights: **NVIDIA Open Model License** ("Licensed by NVIDIA Corporation under the NVIDIA Open Model
  License"); code: Apache-2.0. The ONNX conversion is hosted in `fernandotonon/QtMeshEditor-models` with both texts.
- Text encoder: `Llama-3-LLM2Vec-8B-Kimodo-TextEncoder` — a **Meta Llama 3** derivative under the **Meta Llama 3
  Community License**; use must follow the Llama 3 Acceptable Use Policy. "Meta Llama 3 is licensed under the Meta
  Llama 3 Community License, Copyright © Meta Platforms, Inc. All Rights Reserved." The merged LLM2Vec adapters are
  MIT. Hosted in `fernandotonon/Llama-3-LLM2Vec-8B-Kimodo-TextEncoder-GGUF`; the licence, the policy and the Notice
  download with it.
- Post-processing: NVIDIA MotionCorrection (Apache-2.0), vendored; its notices ship as
  `kimodo_motion_correction.THIRD_PARTY_LICENSES.txt` next to the editor.

Details: `THIRD_PARTY_AI_MODELS.md`. Maintainer notes (export, parity, hosting): `CLAUDE.md`, `ai/kimodo/README.md`.
