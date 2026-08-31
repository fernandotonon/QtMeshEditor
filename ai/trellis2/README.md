# QtMeshEditor TRELLIS.2 sidecar

Out-of-process Python runtime for the `trellis2` image-to-3D backend
(`qtmesh generate3d --backend trellis2`, the "TRELLIS.2 — High Quality" option in the
GUI's *AI: Image → 3D* panel).

## What it does — and what it deliberately does NOT do

`generate.py` runs **Microsoft TRELLIS.2** (MIT, pinned revision) inference only:

```
RGBA image (alpha matte made by QtMeshEditor's own U²-Net)
   ↓ generate.py            DINOv3 cond → sparse structure → shape SLat → tex SLat
raw geometry + sparse PBR attribute volume (base color / metallic / roughness / alpha)
   ↓ QTM3D interchange file (qtm3d.py ↔ src/ImageTo3D/Trellis2Interchange.*)
QtMeshEditor C++: cleanup → weld → simplify (game-ready presets) → xatlas UV
   → texture/PBR bake (own rasterizer) → normals/tangents → Ogre / GLB / FBX
```

The upstream reference implementation uses **NVIDIA nvdiffrast + nvdiffrec** for UV-space
rasterization, texture baking and PBR previews. Those libraries are under the NVIDIA Source
Code License (research/evaluation only) and are **prohibited here**: they are not in
`requirements.txt`, `install.py` never installs them, `generate.py` never imports them (and
warns — `--strict`: fails — if they are unexpectedly present), and `install.py` patches the
MIT file `o_voxel/__init__.py` so the o-voxel package imports without them. The equivalent
functionality is QtMeshEditor's own C++ code (`src/ImageTo3D/Trellis2Bake.*`). Full audit:
`docs/trellis2-dependencies.md`.

The upstream default background remover (`briaai/RMBG-2.0`, named in the shipped
`pipeline.json`) is **CC BY-NC** and is likewise never downloaded or loaded — the loader is
stubbed and inputs must already carry an alpha matte.

## Requirements

- Linux, NVIDIA GPU (**≥ 24 GB VRAM** recommended; `low_vram` staggering is on), CUDA 12.4
- Python ≥ 3.10, git, a compiler toolchain (the o-voxel/CuMesh/FlexGEMM CUDA extensions are
  built from pinned sources)
- A Hugging Face account that has **accepted Meta's DINOv3 License** (the
  `facebook/dinov3-vitl16-pretrain-lvd1689m` encoder is gated): `huggingface-cli login`
- ~19 GB for TRELLIS.2-4B weights (MIT), downloaded on first generation

## Install

```bash
python3 ai/trellis2/install.py                 # → <app data>/QtMeshEditor/trellis2
# or choose a location:
python3 ai/trellis2/install.py --dest /opt/qtmesh-trellis2
```

QtMeshEditor auto-detects the default location. For a custom one, set the env var
`QTMESH_TRELLIS2_ENV=<dest>` or QSettings `ai/trellis2Env`.

## Direct use / troubleshooting

```bash
ENV=~/.local/share/QtMeshEditor/trellis2
$ENV/env/bin/python $ENV/generate.py --check                 # environment probe
$ENV/env/bin/python $ENV/generate.py --report-deps --check   # dependency report
$ENV/env/bin/python $ENV/generate.py --input subject.png --output out.qtm3d \
    --preset balanced --seed 42
$ENV/env/bin/python $ENV/generate.py --input any.png --output out.qtm3d --mock
                                                             # plumbing test, no GPU
```

Presets: `fast` = TRELLIS.2 `512`, `balanced` = `1024_cascade` (upstream default),
`high` = `1536_cascade`.

Progress/status is emitted as JSON lines on stdout (QtMeshEditor parses these); tqdm and
debug logs go to stderr.

## Licenses

See `THIRD_PARTY_LICENSES.md` (this directory) and `docs/trellis2-dependencies.md` for the
authoritative table. Summary: TRELLIS.2 code + weights MIT; CuMesh/FlexGEMM MIT; PyTorch /
flash-attn BSD-3; transformers Apache-2.0; DINOv3 encoder under Meta's **DINOv3 License**
(commercial use permitted, gated download, "Built with DINOv3" attribution); easydict
LGPL-3.0 (pure-Python, flagged); nvdiffrast/nvdiffrec/RMBG-2.0 excluded.

*Built with DINOv3.*
