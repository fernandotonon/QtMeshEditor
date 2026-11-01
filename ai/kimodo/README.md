# QtMeshEditor Kimodo sidecar

**Built with Meta Llama 3.**

> QtMeshEditor now runs Kimodo **natively** (no Python): `src/AIMotion/KimodoNative.cpp` runs the text encoder
> through the bundled llama.cpp and the motion model through ONNX Runtime, matching Kimodo's own PyTorch output on
> the same noise (max joint rotation error 2e-4, root 0.003 cm, identical foot contacts). Its ~9 GB of models
> download once on request: `qtmesh anim <rig> --generate "<prompt>" --backend kimodo --download`, MCP
> `generate_motion {backend:"kimodo", download:true}`, or the Inspector's "Download Kimodo models" button.
> `--runtime auto|native|python` picks the runtime (auto = native when downloaded, else this sidecar). This
> sidecar remains for constraints files and as an alternative runtime.

Out-of-process Python runtime for the `kimodo` text-to-motion backend
(`qtmesh anim <rig> --generate "<prompt>" --backend kimodo --runtime python`).

`generate.py` runs **NVIDIA Kimodo** inference only and writes QtMeshEditor's motion
interchange NPZ (SOMA-77 skeleton + parent-relative rotations + root path + foot contacts).
Everything after that is QtMeshEditor C++ (`src/AIMotion/`): the versioned SOMA -> standard
bone map (`SomaSkeleton.h`), the retarget onto the user's rig (`SomaRetarget.h`, the #523
incompatible-skeleton retargeter), keyframes, foot pinning and export. Python is never
needed to start the editor; the runtime is probed only when a Kimodo generation is requested.

## Licences

| Component | Licence |
|---|---|
| Kimodo code (`nv-tlabs/kimodo`, pinned in `install.py`) | Apache-2.0 |
| `nvidia/Kimodo-SOMA-RP-v1.2` weights | NVIDIA Open Model License (commercial use allowed) |
| `McGill-NLP/LLM2Vec-Meta-Llama-3-8B-Instruct-mntp(-supervised)` adapters | MIT |
| `meta-llama/Meta-Llama-3-8B-Instruct` text encoder | Meta Llama 3 Community License, **gated** |
| sse2neon (ARM builds only, pinned + SHA-256 verified) | MIT |

The research-only `Kimodo-SMPLX-RP` checkpoint (NVIDIA R&D licence) is never used:
`generate.py` refuses any non-SOMA model. No weights are bundled or mirrored; they download
from their publishers on first use, with the user's own Hugging Face login.

## Setup

```bash
python3 ai/kimodo/install.py                 # Linux/Windows x86
python3 ai/kimodo/install.py --arm-sse2neon  # Apple Silicon / ARM
hf auth login                                # after accepting Meta's Llama 3 licence on Hugging Face
```

The Llama encoder is ~16 GB on disk. `generate.py` encodes the prompts first and frees the encoder before the diffusion
model loads, so the two never share memory.

## Text encoder quantization (default: int8)

`--text-encoder-quant auto|none|int8|int4` (env `QTMESH_KIMODO_TEXTENC_QUANT`; CLI `--text-encoder-quant`). `auto` = int8:
on the first generation `textenc_quant.py` builds a weight-only quantized copy of the encoder ONCE from the user's own
Llama download (base + LLM2Vec LoRA merged, every `nn.Linear` to int8 with one fp16 scale per row and 128-column group)
into `<runtime>/textenc_int8/` (~8 GB, ~1-2 min, ~8.5 GB RAM while building); later runs load it instead. Dequantization
is plain PyTorch, so it runs on MPS, CUDA and CPU (bitsandbytes has no MPS backend). Measured on an M-series Mac (24 GB),
24 prompts, against the bf16 encoder:

| encoder | embedding cosine (min) | rel. L2 | GPU memory | load (warm) |
|---|---|---|---|---|
| bf16 (`none`) | 1 | 0 | 15.2 GB | 79 s |
| **int8** (default) | **0.9998** | **1.8 %** | **8.7 GB** | **8 s** |
| int4 | 0.967 | 24 % | 5.6 GB | 4 s |

The quantized checkpoint is a derivative of Meta Llama 3 built from the user's own gated download: it stays on the user's
machine and is never bundled, mirrored or uploaded. `QTMESH_KIMODO_TEXTENC_DIR` points at a checkpoint elsewhere.

Trap found building it: LLM2Vec chooses the prompt template from `config._name_or_path`, and only the exact string
`meta-llama/Meta-Llama-3-8B-Instruct` gets the Llama 3 chat header. A checkpoint reloaded from a folder carries the folder
name and silently encodes WITHOUT the header (cosine 0.94 with identical weights), so the original name is stored in
`quant.json` and restored on load.

## Test without weights

`QTMESH_KIMODO_MOCK=1` (or `generate.py --mock`) writes a synthetic walk on the real SOMA-77
rest skeleton with numpy only, which exercises the whole C++ path:

```bash
QTMESH_KIMODO_MOCK=1 QTMESH_KIMODO_PYTHON=$(which python3) QTMESH_KIMODO_GENERATE=$PWD/ai/kimodo/generate.py \
  qtmesh anim rig.fbx --generate "a person walks forward" --backend kimodo --variations 2 -o out.fbx --json
```

`qtmesh anim rig.fbx --apply-motion motion_00.npz -o out.fbx` re-applies a saved interchange
file to any rig with no Python at all.
