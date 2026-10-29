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
*generate from text (NVIDIA Kimodo)*. Defaults: 2 s, 1 variation, root motion off (the clip stays in place).
One prompt per line makes a continuous multi-step motion; `@SECONDS` at the end of a line sets that step's length:

```
walk forward @3
sit down @2
```

Every variation becomes its own clip (`generated_kimodo`, `generated_kimodo_v2`, …), added in one undo step.

## CLI

```bash
qtmesh anim rigged.glb --generate "a person jumps forward" --backend kimodo --duration 2 -o out.glb
qtmesh anim rigged.glb --backend kimodo --segment "walk forward@3" --segment "sit down@2" -o out.glb
qtmesh anim rigged.glb --generate "wave" --backend kimodo --variations 3 --seed 7 --no-root-motion -o out.glb
qtmesh anim rigged.glb --apply-motion motion_00.npz -o out.glb   # re-apply a saved motion, any rig, no model
```

Options: `--runtime auto|native|python` (auto = native when its models are downloaded, else the sidecar),
`--download`, `--variations 1..16`, `--seed N`, `--diffusion-steps 1..1000` (default 100), `--no-root-motion`,
`--no-postprocess`, `--name CLIP`, `--json`. Kimodo-only options are refused without `--backend kimodo`.
`--constraints` (Kimodo constraint files) needs the Python runtime.

## MCP

```json
{"name": "generate_motion", "arguments": {"backend": "kimodo", "prompt": "a person jumps forward", "duration": 2,
  "variations": 1, "root_motion": false, "download": true, "output_path": "/tmp/jump.glb"}}
```

Also `segments`, `seed`, `diffusion_steps`, `postprocess`, `runtime`, `clip_name`, and `motion_files` (re-apply saved
motions). Options are validated before any download; Stop interrupts a download or a generation; a failed export
takes the new clips back out of the scene.

## Speed and accuracy

On an Apple M-series Mac, ~18 s per 5 s variation at the default 100 steps, plus ~4 s to load the models and encode the prompt (fewer `diffusion_steps` is faster). The
native runtime reproduces Kimodo's own PyTorch output on the same inputs to a 2e-4 maximum joint-rotation
difference and 0.003 cm root difference, with identical foot contacts (`scripts/export-kimodo-onnx.py --verify`).

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
