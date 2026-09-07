# TRELLIS.2 image-to-3D backend

TRELLIS.2 (Microsoft, MIT code + MIT weights) is QtMeshEditor's highest-quality
image-to-3D backend, and the **default** one whenever its runtime is installed. It sits
next to the local ONNX backends:

| Backend | Runs | Quality | Output |
|---|---|---|---|
| **TRELLIS.2** | local Python sidecar, Linux + NVIDIA GPU (≥24 GB VRAM rec.) | highest | full PBR (base color + metallic + roughness + alpha), game-ready presets |
| TripoSR | in-process ONNX, any machine | fast tier | diffuse (+#404 synthesized PBR) |
| TripoSG | in-process ONNX, any machine | best local geometry | geometry (+AI texture pass) |

## Architecture — who does what

**TRELLIS.2 generates; QtMeshEditor makes the asset.**

```text
input image ── U²-Net alpha matte (QtMeshEditor, Apache-2.0) ──► RGBA
    ──► ai/trellis2/generate.py  (inference ONLY: DINOv3 cond → sparse structure
         → shape SLat → tex SLat → raw mesh + sparse PBR attribute volume)
    ──► QTM3D interchange file (Trellis2Interchange)
    ──► QtMeshEditor C++ (Trellis2Bake):
           weld → remove debris components → simplify (game-ready presets)
           → xatlas UV unwrap → rasterize charts → project each texel to the
             closest point on the full-res source surface → trilinearly sample
             the attribute volume → bake base color (RGBA) + roughness +
             metallic + tangent-space normal map → dilate seams
    ──► Ogre scene / GLB / FBX / any Assimp-supported export
```

The full-resolution generation is preserved as a `*_source.qtm3d` sidecar (next to CLI/MCP
exports; under `<AppData>/generated_sources/` for GUI runs) so textures and LODs can be
re-baked later without re-running inference.

### The NVIDIA exclusion

The upstream reference implementation uses **nvdiffrast** (UV rasterization/texture bake,
preview rendering) and **nvdiffrec** (environment-light PBR previews). Both are under the
NVIDIA Source Code License — *research or evaluation use only* — which is incompatible
with QtMeshEditor's commercial redistribution, so this integration **excludes them
entirely**: never installed, never imported, never invoked; the sidecar warns if they are
unexpectedly present, `scripts/check-trellis2-restricted-deps.sh` + `Trellis2GuardTest`
gate CI, and everything they did is replaced by QtMeshEditor's own code
(`src/ImageTo3D/Trellis2Bake.{h,cpp}` — xatlas, meshoptimizer, a conventional barycentric
UV-space rasterizer, Ericson closest-point queries and trilinear sparse-volume sampling;
previews come from the existing Ogre/RTSS + HDR/IBL renderer). Two more license traps are
bypassed: the upstream default background remover `briaai/RMBG-2.0` is **CC BY-NC** and is
never downloaded (QtMeshEditor's own U²-Net produces the alpha matte), and `pip install
cumesh` would fetch an unrelated unlicensed PyPI package (CuMesh is built from the pinned
MIT checkout instead).

**Accurate license statement:** TRELLIS.2 code and TRELLIS.2-4B weights are MIT; the
required DINOv3 image encoder is under Meta's **DINOv3 License** (commercial use permitted,
gated download under the user's own HF account, "Built with DINOv3" attribution); the
easydict transitive dependency is LGPL-3.0 (pure-Python). It would be wrong to call the
whole stack "entirely MIT". Full audit: [`docs/trellis2-dependencies.md`](trellis2-dependencies.md).

## Runtime flavors

The backend has **two interchangeable runtimes**; `Trellis2Predictor` prefers trellis.cpp
when both are present:

| Flavor | Stack | Platforms | Discovery |
|---|---|---|---|
| **trellis.cpp** (preferred, **bundled**) | C++/GGML ([pwilkin/trellis.cpp](https://github.com/pwilkin/trellis.cpp)) — CUDA / Vulkan / **Metal** / CPU, no Python, GGUF weights. Built with `ENABLE_TRELLIS_CPP` as a **self-contained** `trellis-cli` (`BUILD_SHARED_LIBS=OFF` so ggml is linked in — shipping only the CLI without `libggml*.so` caused exit 127 on Linux .deb/snap). Linux x86_64 ships an **AVX-without-FMA** ggml baseline (no AVX2/FMA/F16C) so pre-Haswell CPUs like Ivy Bridge do not SIGILL. | Linux + **macOS** release builds (Windows MinGW still OFF pending verification) | Discovery order: env `QTMESH_TRELLIS2_CLI` → QSettings `ai/trellis2Cli` → bundled next to the editor binary → `trellis-cli` on `PATH`. Models: `QTMESH_TRELLIS2_CLI_MODELS` / `ai/trellis2CliModels` / `<AppData>/ai_models/trellis2` (GGUFs from AI Model Settings / [`ilintar/trellis2-gguf`](https://huggingface.co/ilintar/trellis2-gguf)) |
| Python sidecar | upstream-exact PyTorch/CUDA (`ai/trellis2/`) | Linux + NVIDIA | as below |

QtMeshEditor invokes `trellis-cli --dump-post` (upstream PR
[#45](https://github.com/pwilkin/trellis.cpp/pull/45); pin must be at/after that merge —
3.37.6 shipped an older pin and every install failed with `unknown option: --dump-post`):
trellis.cpp emits the RAW decoded mesh + sparse PBR volume and exits before its own
remesh/UV/bake — QtMeshEditor keeps the game-ready + native-bake pipeline either way.
Presets map to `--res` (fast=512, balanced=1024, high=1536); with only the 512 GGUFs
installed the backend drops to the 512 pipeline with a warning. `--mock` runs always
route to the Python sidecar.

## Install (Linux + NVIDIA GPU)

```bash
python3 ai/trellis2/install.py            # → ~/.local/share/QtMeshEditor/trellis2
huggingface-cli login                     # DINOv3 is gated — accept Meta's terms on HF first
```

QtMeshEditor auto-detects the default location; override with `QTMESH_TRELLIS2_ENV` (or
QSettings `ai/trellis2Env`), and the interpreter with `QTMESH_TRELLIS2_PYTHON`
(`ai/trellis2Python`). The ~19 GB TRELLIS.2-4B weights download on the first generation.
Everything is isolated in that directory — nothing touches QtMeshEditor's own dependencies.

Health checks:

```bash
$ENV/env/bin/python $ENV/generate.py --check                # environment probe
$ENV/env/bin/python $ENV/generate.py --check --report-deps  # Phase 15 dependency report
```

The report prints the loaded stack, ending with
`nvdiffrast: NOT INSTALLED / NOT USED` / `nvdiffrec: NOT INSTALLED / NOT USED`.

## Use

**GUI:** Object mode → Mode Tools → *AI: Image → 3D*. The Backend combo lists
*TRELLIS.2 (high quality)* first and preselects it when the runtime is present. Options:
Quality (*Fast* = 512 / *Balanced* = 1024 cascade / *High* = 1536 cascade), Mesh
(*Original*, *Game Low ~10k*, *Game Medium ~25k*, *Game High ~50k* triangles), Texture
(1024/2048/4096), plus the shared Remove-background / Bake / PBR / Upscale toggles.

**CLI:**

```bash
qtmesh generate3d photo.png -o out.glb                       # trellis2 when installed, else triposr
qtmesh generate3d photo.png -o out.glb --backend trellis2 \
    --preset high --target-tris 25000 --texture-size 4096 --seed 7
```

**MCP:** `generate_mesh_from_image` with `backend: "trellis2"` (`seed`, `preset`,
`target_tris` args; the response carries `backend` and `sourcePath`).

## Errors you may see

- *"TRELLIS.2 runtime not installed"* — run `ai/trellis2/install.py` or point
  `QTMESH_TRELLIS2_ENV` at it.
- *"CUDA GPU not available"* — the sidecar needs an NVIDIA GPU; use TripoSR/TripoSG locally.
- Hugging Face 401/403 on DINOv3 — accept the DINOv3 License on HF and `huggingface-cli login`.
- *"input image has no alpha matte"* — enable Remove background (or supply an RGBA image).
- Python stack traces stay on stderr (developer log); surfaces show the single structured
  error message.

## Testing without a GPU

The sidecar's `--mock` mode (or env `QTMESH_TRELLIS2_MOCK=1`) generates a synthetic
sphere + attribute volume with no torch/CUDA, exercising the exact interchange →
game-ready → bake → export path end-to-end. Unit tests: `Trellis2Interchange_test.cpp`,
`Trellis2Bake_test.cpp`, `Trellis2Predictor_test.cpp`, `Trellis2Guard_test.cpp`.

*Built with DINOv3.*
