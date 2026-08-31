# TRELLIS.2 integration — dependency & license audit

Status: **authoritative record** for the `qtmesh generate3d --backend trellis2` integration.
Audited: 2026-08-30, against the pinned upstream revisions below. Re-audit whenever a pin moves.

QtMeshEditor's TRELLIS.2 integration is designed around one hard constraint:

> **NVIDIA nvdiffrast and nvdiffrec are excluded — not installed, not imported, not
> dynamically loaded, not bundled, not invoked indirectly, and not ported/translated.**
> Both are under the *NVIDIA Source Code License*, which limits use to
> "research or evaluation purposes only" for everyone but NVIDIA. That is incompatible
> with QtMeshEditor's permissive commercial redistribution (Homebrew / Snap / WinGet /
> Docker / Marketplace).

Everything those libraries do for the upstream reference implementation (UV-space
rasterization, texture baking, PBR preview rendering) is performed by **QtMeshEditor's own
C++ code** (`src/ImageTo3D/Trellis2Bake.{h,cpp}`, xatlas + meshoptimizer + a conventional
scanline/barycentric rasterizer and trilinear sparse-volume sampler — standard, publicly
documented graphics algorithms; no NVIDIA source was read, copied, or translated for it).

---

## 1. Pinned upstream revisions

| Component | Source | Pinned revision | License |
|---|---|---|---|
| TRELLIS.2 code (incl. in-repo `o-voxel` package) | <https://github.com/microsoft/TRELLIS.2> | `75fbf0183001ed9876c8dbb35de6b68552ee08bd` (main, 2026-06-05) | **MIT** |
| TRELLIS.2-4B weights (9 safetensors, ≈18.9 GB) | <https://huggingface.co/microsoft/TRELLIS.2-4B> | `af44b45f2e35a493886929c6d786e563ec68364d` | **MIT** (model card `license: mit`) |
| TRELLIS 1 sparse-structure decoder (`ss_dec_conv3d_16l8_fp16`, pulled by `pipeline.json`) | <https://huggingface.co/microsoft/TRELLIS-image-large> | `25e0d31ffbebe4b5a97464dd851910efc3002d96` | **MIT** |
| CuMesh | <https://github.com/JeffreyXiang/CuMesh> | pinned in `ai/trellis2/install.py` | **MIT** |
| FlexGEMM | <https://github.com/JeffreyXiang/FlexGEMM> | pinned in `ai/trellis2/install.py` | **MIT** |
| DINOv3 image encoder (`facebook/dinov3-vitl16-pretrain-lvd1689m`) | <https://huggingface.co/facebook/dinov3-vitl16-pretrain-lvd1689m> | HF-gated; downloaded by the user's own HF account | **DINOv3 License** (Meta, custom — see §4) |

> ⚠️ `cumesh` on PyPI (0.1.0, author "Congjie He") is an **unrelated project with no license**.
> Never `pip install cumesh` — `install.py` builds JeffreyXiang/CuMesh from source at the pin.

## 2. Runtime dependency table

"Redistributed" = shipped inside QtMeshEditor binaries/packages. **Nothing in this table is
redistributed** — the whole Python environment is user-installed into
`<AppData>/trellis2/` by `ai/trellis2/install.py` (the same "downloads on first use" stance
as every other AI model in this project). "Required" = required at runtime for the TRELLIS.2
backend specifically (all other QtMeshEditor features work without any of this).

| Dependency | Purpose | Version/pin | License | Redistributed | Downloaded separately | Required at runtime | Commercial use |
|---|---|---|---|---|---|---|---|
| TRELLIS.2 (`trellis2` pkg) | generation pipeline | `75fbf018…` | MIT | no | yes (git) | yes | ✅ |
| `o-voxel` (in-repo) | flexible-dual-grid → mesh extraction, sparse attr volume | same repo pin | MIT (repo LICENSE; vendors Eigen, MPL-2.0) | no | yes | yes | ✅ |
| CuMesh | GPU mesh cleanup (`fill_holes` in `decode_latent`), simplify | install.py pin | MIT | no | yes (git) | yes | ✅ |
| FlexGEMM | sparse conv backend + `grid_sample_3d` | install.py pin | MIT | no | yes (git) | yes | ✅ |
| PyTorch + torchvision (CUDA) | tensor runtime | 2.6.0 / 0.21.0 cu124 | BSD-3-Clause | no | yes (pip) | yes | ✅ |
| flash-attn | default attention backend | 2.7.3 | BSD-3-Clause | no | yes (pip) | yes (or xformers) | ✅ |
| xformers | alternative attention backend | optional | BSD-3-Clause | no | optional | no | ✅ |
| transformers | loads DINOv3 | setup.sh floating; install.py pins | Apache-2.0 | no | yes | yes | ✅ |
| huggingface_hub | weight downloads | (transformers dep) | Apache-2.0 | no | yes | yes | ✅ |
| numpy, pillow | array/image IO | — | BSD / HPND | no | yes | yes | ✅ |
| easydict | config dicts (used by `o_voxel.rasterize`) | 1.13 | **LGPL-3.0** ⚠️ | no | yes | yes (transitive import) | ✅ (pure-Python, imported unmodified from a user-installed env; flagged as the only copyleft item — see §5) |
| trimesh | upstream GLB assembly | not installed | MIT | no | no | **no** (QtMeshEditor writes the asset) | ✅ |
| utils3d | upstream renderers/datasets only | not installed | MIT | no | no | **no** | ✅ |
| opencv / imageio / imageio-ffmpeg / gradio / kornia / timm / lpips / pandas / tensorboard | upstream demos, training, video previews | not installed | various permissive (+LGPL ffmpeg binary) | no | no | **no** | n/a |
| spconv | alternative sparse-conv backend | not installed | Apache-2.0 | no | no | no (default is FlexGEMM) | ✅ |
| kaolin, diffusers, rembg (PyPI) | **not used anywhere** by TRELLIS.2 | — | — | no | no | no | n/a |
| **nvdiffrast** | upstream UV rasterization in `o_voxel.postprocess.to_glb` + preview renderers | **EXCLUDED** | NVIDIA Source Code License (1-Way Commercial) — *"non-commercially… research or evaluation purposes only"* | **never** | **never** | **no — replaced by `Trellis2Bake`** | ❌ |
| **nvdiffrec** (`nvdiffrec_render` fork) | upstream env-map lighting for PBR previews (`pbr_mesh_renderer`) | **EXCLUDED** | NVIDIA Source Code License for nvdiffrec (same restriction) | **never** | **never** | **no — QtMeshEditor's HDR/IBL renderer covers previews** | ❌ |

### Model weights

| Weights | Purpose | License | Bundled? | Commercial use |
|---|---|---|---|---|
| `microsoft/TRELLIS.2-4B` (rev `af44b45f…`) | flow models + shape/tex VAEs | MIT | no — HF download on install | ✅ |
| `microsoft/TRELLIS-image-large` `ss_dec_conv3d_16l8_fp16` only (rev `25e0d31f…`) | sparse-structure decoder referenced by `pipeline.json` | MIT | no — HF download | ✅ |
| `facebook/dinov3-vitl16-pretrain-lvd1689m` | image conditioning encoder | **DINOv3 License** (custom Meta) | no — **gated** HF download by the user | ✅ with conditions (§4) |
| `briaai/RMBG-2.0` | upstream default background remover (named in `pipeline.json`) | **CC BY-NC 4.0** ❌ | **never downloaded or loaded** — bypassed (§3) | ❌ non-commercial |
| `ZhengPeng7/BiRefNet` | MIT alternative background remover | MIT | not used (QtMeshEditor does its own bg removal) | ✅ |
| U²-Net (`u2net.onnx`, already shipped-on-demand by QtMeshEditor #764) | the background removal actually used | Apache-2.0 code, permissive weights | existing on-demand download | ✅ |

## 3. Where nvdiffrast/nvdiffrec live upstream, and how each use is avoided

Complete grep of the pinned revision (`grep -rn 'nvdiffrast\|nvdiffrec' --include=*.py`):

| Upstream site | What it does | How QtMeshEditor avoids it |
|---|---|---|
| `o-voxel/o_voxel/postprocess.py` (top-level `import nvdiffrast.torch`) | `to_glb()`: rasterize UV atlas → texel 3D positions → BVH snap to hi-res mesh → trilinear volume sample → trimesh GLB | **Never called.** `generate.py` exports raw vertices/faces + the sparse attribute volume to the QTM3D interchange; UV unwrap, rasterization, sampling and baking happen in C++ (`Trellis2Bake`). `install.py` patches the MIT file `o_voxel/__init__.py` to import `postprocess` lazily, so `import o_voxel` no longer requires nvdiffrast to be installed at all. |
| `trellis2/renderers/mesh_renderer.py`, `pbr_mesh_renderer.py` (lazy imports; `pbr_mesh_renderer` also imports `nvdiffrec_render.light`) | turntable/PBR preview videos | **Never imported** — `trellis2/renderers/__init__.py` is lazy (`__getattr__`); `generate.py` never touches renderers. Previews come from QtMeshEditor's own Ogre/RTSS + HDR/IBL pipeline. |
| `trellis2/pipelines/trellis2_texturing.py` (top-level import) | the separate "texture an existing mesh" pipeline | **Never imported** — `trellis2/pipelines/__init__.py` is lazy; only `Trellis2ImageTo3DPipeline` is loaded. |
| `example.py` / `app.py` | demo scripts (comment: 2^24-vertex nvdiffrast limit) | not used. |

The core generation chain — image → DINOv3 cond → sparse-structure flow → shape SLat flow →
`shape_slat_decoder` → `o_voxel.convert.flexible_dual_grid_to_mesh` (Eigen QEF dual grid,
**not** FlexiCubes; zero NVIDIA code, verified by grep for `flexicubes` and NVIDIA copyright
headers) → tex SLat flow → `tex_slat_decoder` → `MeshWithVoxel` — **contains no nvdiffrast or
nvdiffrec call**. `generate.py` refuses to start if either module is importable in strict
mode, and reports their absence in its dependency report (Phase 15).

### RMBG-2.0 (non-commercial) bypass

`TRELLIS.2-4B/pipeline.json` sets the rembg model to `briaai/RMBG-2.0` (CC BY-NC 4.0), and
the upstream `BiRefNet.__init__` downloads it **eagerly at pipeline construction**. The
QtMeshEditor sidecar therefore:

1. replaces `trellis2.pipelines.rembg.BiRefNet` with an inert stub **before**
   `from_pretrained` runs, so the weights are never downloaded or loaded;
2. always feeds the pipeline an **RGBA image whose alpha was produced by QtMeshEditor's own
   U²-Net background remover** (Apache-2.0, already part of #764) — upstream's
   `preprocess_image()` uses a supplied alpha channel directly and never calls the rembg
   model on such input.

## 4. DINOv3 — the one non-MIT required model (be precise about this)

The shipped TRELLIS.2-4B `pipeline.json` conditions on
`facebook/dinov3-vitl16-pretrain-lvd1689m` loaded via `transformers.DINOv3ViTModel`. The
checkpoint was trained against this embedding space; **swapping in CLIP/DINOv2 would break
generation**, so it is not replaced.

- License: **DINOv3 License** (Meta, custom): commercial use **permitted**; redistribution
  permitted **with conditions** (include the license, display **"Built with DINOv3"**,
  derivatives inherit the license); acceptable-use policy and export-control restrictions
  apply; the HF repo is **gated** (user must accept terms and use their own HF token).
- QtMeshEditor **does not redistribute** DINOv3. `install.py`/first run download it under the
  *user's* HF account after they accept Meta's terms. The "Built with DINOv3" notice appears
  in `ai/trellis2/THIRD_PARTY_LICENSES.md` and `docs/TRELLIS2.md`.
- Consequence: it is **incorrect** to describe the TRELLIS.2 integration as "entirely MIT".
  The accurate statement is: *TRELLIS.2 code and weights are MIT; the required DINOv3 image
  encoder is under Meta's DINOv3 License (commercial use permitted, gated download,
  attribution required); NVIDIA nvdiffrast/nvdiffrec are excluded entirely.*

## 5. Flagged items (yellow)

- **easydict (LGPL-3.0)** — pure-Python, imported unmodified from the user-installed
  environment (the standard LGPL-compliant usage pattern for interpreted code); the only
  copyleft item in the runtime set. It is required because `o_voxel.rasterize` imports it at
  module level. Tracked follow-up: upstream a lazy import or vendor a ~20-line permissive
  replacement into the o_voxel patch if this ever becomes a distribution concern.
- **imageio-ffmpeg** — not installed (video previews are not used); noted only because
  upstream's `setup.sh` installs it.
- **DINOv3** — see §4.

## 6. Prohibited-dependency enforcement

- `ai/trellis2/requirements.txt` and `install.py` never reference nvdiffrast/nvdiffrec
  (except in prohibition comments).
- `generate.py` startup check: warns (and in `--strict` mode refuses to run) if
  `nvdiffrast` or `nvdiffrec`/`nvdiffrec_render` is importable in the environment; its
  `--report-deps` output prints `nvdiffrast: NOT INSTALLED / NOT USED`.
- `scripts/check-trellis2-restricted-deps.sh` — CI grep gate over `ai/trellis2/` and the
  C++ integration sources; fails on any non-allowlisted mention.
- `Trellis2GuardTest` (Google Test, runs in the normal Linux CI test job) re-checks the same
  invariants from the built test binary.

Allowed mentions of the two names: license documentation (this file,
`ai/trellis2/THIRD_PARTY_LICENSES.md`, `docs/TRELLIS2.md`, `THIRD_PARTY_AI_MODELS.md`),
prohibition comments/guards in `ai/trellis2/*.py`, the CI check script, and the guard test.

## 7. GPU / platform requirements (upstream)

Linux + NVIDIA GPU with **≥ 24 GB VRAM** (verified on A100/H100 upstream; `low_vram: true`
staggers models CPU↔GPU), CUDA 12.4, PyTorch 2.6.0+cu124, flash-attn (or xformers). No CPU,
no Apple Silicon path — on machines without a suitable GPU the backend reports "runtime not
available" and QtMeshEditor's TripoSR/TripoSG ONNX backends remain the local option.
Using CUDA/PyTorch is fine license-wise (BSD-3); the exclusion above concerns only the
research-only NVIDIA libraries, not ordinary GPU runtimes.
