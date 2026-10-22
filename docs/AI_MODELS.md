# AI models used in QtMeshEditor

An at-a-glance index of every AI model (and model-like asset) the project uses:
what it is, what it does inside QtMeshEditor, and under what licence.

- **Nothing is bundled in the binary.** Models download on first use (or from
  *AI Model Settings*) into `<AppData>/ai_models/…`, unless noted. Most are
  mirrored on the `fernandotonon/QtMeshEditor-models` Hugging Face repo.
- **Everything has a fallback or a clear error.** Where a deterministic
  fallback exists (geometric segmentation, spline in-betweening, template
  motion, Pinocchio-style rigging…) it is noted.
- This page is a summary. **Authoritative licence analysis, training-data
  provenance, export contracts and rejected alternatives** live in
  [`THIRD_PARTY_AI_MODELS.md`](../THIRD_PARTY_AI_MODELS.md). Check there before
  redistributing anything.
- Licences marked *(see upstream)* were not re-verified when this page was
  written.

Runtimes: **ONNX Runtime** (`ENABLE_ONNX`), **llama.cpp** (`ENABLE_LOCAL_LLM`,
GGUF), **stable-diffusion.cpp** (`ENABLE_STABLE_DIFFUSION`), **trellis.cpp**
(out-of-process helper) and a Python sidecar for the TRELLIS.2 CUDA path.

---

## 1. Image / text → 3D mesh

| Model | What it is | Used in QtMeshEditor for | Licence |
|---|---|---|---|
| **TRELLIS.2-4B** (Microsoft) | 4B sparse-voxel flow model producing geometry plus real PBR (base colour, metallic, roughness, alpha). Runs via trellis.cpp (GGUF) or a CUDA Python sidecar. | Highest-quality `generate3d` / "AI: Image → 3D" backend, the default when installed. QtMeshEditor adds its own alpha matte, game-ready simplify, xatlas UV and PBR bake. | MIT (code + weights) |
| **Pixal3D** (TencentARC) | TRELLIS.2 fork using view-aligned projection conditioning; reuses TRELLIS.2 decoders. | Alternative `generate3d --backend pixal3d`, worth trying on characters. | MIT |
| **TripoSG** (VAST-AI) | 1.5B rectified-flow DiT over an SDF VAE (4 ONNX graphs). Geometry only. | `--backend triposg`: higher-fidelity geometry than TripoSR, slower. Colour is baked from the input photo + TripoSR's colour field. | MIT |
| **TripoSR** (Tripo AI + Stability) | Single-image triplane reconstruction (fp32 / int8 ONNX). | Default fast `generate3d` backend and the colour source for TripoSG. | MIT |
| **U²-Net** | Salient-object segmentation (ONNX). | Removes the background from input photos before 3D reconstruction (`--remove-bg`). | Apache-2.0 |
| **SmolVLM-500M** (+ mmproj) | Small vision-language model (GGUF, via llama.cpp `mtmd`). | Captions reference images in the image-to-3D panel. | *(see upstream; not yet recorded in THIRD_PARTY_AI_MODELS.md)* |
| **FLUX.2-klein-4B** (Black Forest Labs) | 4B distilled text-to-image model (GGUF) with a Qwen3-4B text encoder. | **Default** prompt → source image for prompt-to-3D (`generate3d --prompt`), plus reference-image editing. | Apache-2.0 |
| **FLUX.3 Image Klein** | 5.6B distilled text-to-image model with a Qwen3-VL-4B text encoder. | **Not downloadable from the app.** If you place a licensed component set in `ai_models/flux3_klein/` yourself, it runs as an alternative to FLUX.2-klein (`--image-model FLUX.3-Image-Klein`) through the bundled `qtmesh_sdcli` helper process. | **FLUX Kommunity License v1.0 — not permissive** (free commercial output use only under US$5M revenue; see THIRD_PARTY_AI_MODELS.md) |

## 2. Textures and materials

| Model | What it is | Used in QtMeshEditor for | Licence |
|---|---|---|---|
| **PBRify_Remix** (3 SPAN models) | Normal / roughness / height predictors from one albedo image (ONNX). | "Generate PBR maps from diffuse", `qtmesh material --generate-pbr`, and the PBR stage of `generate3d`. Roughness also has an offline heuristic. | CC0-1.0 |
| **Real-ESRGAN x2plus / x4plus** | Super-resolution (ONNX). | Texture upscaling 2×/4×, and optional upscale of baked 3D-generation textures. | BSD-3-Clause |
| **LaMa** | Large-mask inpainting (ONNX). | Fills masked texture regions, UV-seam gutters after a bake, and the texture-paint *Inpaint* button. | Apache-2.0 (code + weights) |
| **Depth-Anything-V2-Small** | Monocular depth estimation (ONNX). | Depth from a photo (`qtmesh material --photo-depth`) as ControlNet conditioning. | Apache-2.0 (Small only) |
| **Stable Diffusion 1.5 / SDXL Base 1.0** | Classic diffusion checkpoints (`.safetensors`). | AI texture generation in the Material Editor and `qtmesh material --generate-texture`, plus SDXL as a non-distilled image option for prompt-to-3D. | CreativeML OpenRAIL-M (SD 1.5, see upstream) / OpenRAIL++-M (SDXL) |
| **ControlNet v1.1 depth (SD 1.5)** | Depth-conditioned ControlNet. | Mesh-aware texture generation: the mesh's rendered depth map conditions the texture. | CreativeML OpenRAIL-M *(see upstream)* |

## 3. Rigging and skinning

| Model | What it is | Used in QtMeshEditor for | Licence |
|---|---|---|---|
| **UniRig** (VAST-AI / Tsinghua) | Autoregressive transformer predicting a skeleton from geometry (ONNX encoder + decoder). | `qtmesh rig --algo unirig` and the Inspector Rigging section. Falls back to the template rig when unavailable. | MIT (code + weights); trained on Articulation-XL2.0 (CC-BY-4.0) |
| **SkinTokens / TokenRig** (VAST-AI) | Qwen3-0.6B-based learned skin-weight predictor (5 ONNX graphs, ~2.3 GB). | **Default** skinning algorithm (`qtmesh skin`, `auto_rig --skin`). Falls back to geodesic-voxel bind. | MIT |
| **ICT-FaceKit template** | Neutral head + 52 blendshape deltas (a data asset, not a learned model). | Source for deformation transfer in `qtmesh facerig`, auto-generating the 52 ARKit blendshapes on a face mesh. | MIT |

## 4. Animation and motion

| Model | What it is | Used in QtMeshEditor for | Licence |
|---|---|---|---|
| **RMIB in-betweener** (ours, trained on CMU MoCap) | Robust-Motion-In-betweening-style transformer on a 22-joint skeleton (ONNX, ~13 MB). | `qtmesh anim --in-between`, dope-sheet "AI in-between", MCP `motion_in_between`. Falls back to a cubic-Hermite/slerp spline. | Our weights; CMU data is permissive |
| **Motion library** (curated clips, not a model) | CC0 / CC-BY mocap clips on the 22-joint canonical skeleton. | Default text-to-motion: keyword-matched clip retargeted onto the rig (`qtmesh anim --generate`). Credits in `ATTRIBUTION.md`. | CC0 / CC-BY per clip |
| **Text-to-motion CVAE** (ours) | Small transformer CVAE trained on CMU (`t2m.onnx`). | Opt-in `--model` generative path for single-action prompts. Multi-step prompts route to the library. | Our weights; CMU data |
| **Audio2Face-3D v2.3** (NVIDIA) | Speech → facial-motion network (ONNX) projected onto the ARKit-52 basis. | `qtmesh lipsync`, MCP `generate_lipsync`, Inspector "Lipsync from Audio". Hosted by NVIDIA, not mirrored. | NVIDIA Open Model License |
| **MediaPipe Face** (BlazeFace, Face Mesh V2, Blendshapes) | Face detection, 478-landmark mesh, 52 blendshape scores (ONNX). | `qtmesh mocap --face`: facial expression + head pose from video/webcam. | Apache-2.0 |
| **MediaPipe Pose** (BlazePose detector + landmarks) | 33 world-landmark body pose (ONNX). | `qtmesh mocap --body` and still-image pose keyframes (`pose-ik`). | Apache-2.0 |
| **MediaPipe Hand landmarks** | Hand landmark model (ONNX). | Finger/hand tracking in performance capture (`HandCapPredictor`; not a separate catalog entry). | *(see upstream; not yet recorded in THIRD_PARTY_AI_MODELS.md)* |
| **SAM 3D Body** (Meta) | Body mesh-recovery model. | Declared quality path for `mocap --body`, currently falling back to `pose-ik` because the checkpoints are gated. **Not shipped.** | Gated upstream |

## 5. Mesh analysis

| Model | What it is | Used in QtMeshEditor for | Licence |
|---|---|---|---|
| **Mesh segmentation family** (ours) | PointNet++-style point-cloud segmenters: body, vegetation, vehicle, building, plus a 4-way category classifier. | Edit Mode "Select by Part (AI)", `qtmesh segment` / PartOps split-explode, material-per-part and rig priors. Falls back to geometric segmentation. | Our weights, trained on synthetic and permissive (CC0 / CC-BY) data |

## 6. Local LLMs (AI chat and agent)

Run through llama.cpp as single-file **GGUF** models, downloaded from the AI
settings dialog. No model loads at startup; opening the chat dock loads the last
or a recommended one. The catalog is in `LLMManager::populateRecommendedModels`.

| Model | Used for |
|---|---|
| Gemma 3 1B (Q4_K_M) | Smallest option: quick single-step commands. |
| Qwen 3.5 4B (Q4_K_M) | **Recommended** tool-calling model for the agent. |
| Gemma 4 E4B / 12B / 26B-A4B | Larger Gemma options. |
| Qwen 3.5 9B (16 GB machines), Qwen 3.6 27B and 35B-A3B | Higher-quality agent planning on bigger machines. |

What the LLM does in QtMeshEditor:
- The **AI Chat agent** plans multi-step tasks over the MCP tool set (select,
  rig, generate, export…).
- **Material from a description** (Material Editor "Generate",
  `qtmesh material --describe`, MCP `describe_material`).
- English **intent keywords** for non-English requests during tool routing.

Licences vary by family (Gemma terms; Qwen mostly Apache-2.0), so check each
upstream card.

---

## Not learned models, but related assets

| Asset | Role |
|---|---|
| **xatlas** (MIT) | Auto UV unwrap, used by `qtmesh uv --unwrap` and the generation bakes. |
| **meshoptimizer** (MIT) | LOD generation, vertex-cache/overdraw optimisation, game-ready simplify. |
| **QuadriFlow** (BSD-3, out-of-process helper) | Field-aligned quad remesh (`qtmesh retopo --algo quadriflow`). |
| **OpenRigLogic** (MIT) | Reads MetaHuman `.dna` files. The MetaHuman character assets are not redistributed. |

## Adding a new model

1. Pick a permissive licence, or document the exception in
   `THIRD_PARTY_AI_MODELS.md` with the restrictions copied from the licence text.
2. Add a `ModelSpec` to `src/AIModelCatalog.cpp` and a download resolver with an
   env/QSettings override plus a `*_NO_DOWNLOAD` guard.
3. Fetch through `ModelFetch::ensureBlocking`, passing `expectedSha256` when a
   digest is published.
4. Provide a fallback or a clear error when the runtime or model is missing.
5. Add a row to this page.
