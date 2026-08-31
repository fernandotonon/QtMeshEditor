# TRELLIS.2 sidecar — third-party licenses

This environment is installed on the user's machine by `install.py`; QtMeshEditor does not
redistribute any of it. Authoritative audit: `docs/trellis2-dependencies.md` (pinned
revisions, red/yellow flags, enforcement).

| Component | License | Notes |
|---|---|---|
| Microsoft TRELLIS.2 (code, incl. in-repo `o-voxel`; pinned `75fbf018…`) | MIT | `o_voxel/__init__.py` locally patched (lazy `postprocess`) so the package imports without nvdiffrast |
| `microsoft/TRELLIS.2-4B` weights (HF rev `af44b45f…`) | MIT | downloaded on first use |
| `microsoft/TRELLIS-image-large` sparse-structure decoder (HF rev `25e0d31f…`) | MIT | referenced by upstream `pipeline.json` |
| JeffreyXiang/CuMesh (pinned `12289e10…`) | MIT | built from source — never `pip install cumesh` (unrelated unlicensed PyPI package) |
| JeffreyXiang/FlexGEMM (pinned `6dd94a85…`) | MIT | sparse conv + `grid_sample_3d` |
| Eigen (vendored inside o-voxel) | MPL-2.0 | |
| PyTorch 2.6.0 / torchvision 0.21.0 | BSD-3-Clause | CUDA build |
| flash-attn 2.7.3 (or xformers) | BSD-3-Clause | attention backend |
| transformers, huggingface_hub, safetensors | Apache-2.0 | |
| numpy | BSD-3-Clause | |
| Pillow | HPND/MIT-CMU | |
| easydict 1.13 | **LGPL-3.0** | pure-Python, imported unmodified from this user-installed env; the only copyleft item — flagged in the audit |
| ninja, packaging | Apache-2.0 / BSD | build-time |
| `facebook/dinov3-vitl16-pretrain-lvd1689m` | **DINOv3 License** (Meta, custom) | commercial use permitted; gated download under the user's HF account; license text: <https://ai.meta.com/resources/models-and-libraries/dinov3-license/>. **Built with DINOv3.** |

## Intentionally excluded

| Component | License | Why excluded |
|---|---|---|
| nvdiffrast | NVIDIA Source Code License (1-Way Commercial) — research/evaluation only | replaced by QtMeshEditor's own C++ rasterizer/baker (`src/ImageTo3D/Trellis2Bake.*`) |
| nvdiffrec (`nvdiffrec_render`) | NVIDIA Source Code License for nvdiffrec — research/evaluation only | preview lighting replaced by QtMeshEditor's Ogre/RTSS + HDR/IBL renderer |
| `briaai/RMBG-2.0` background remover | CC BY-NC 4.0 | never downloaded/loaded; background removal is done by QtMeshEditor's U²-Net (Apache-2.0) |
