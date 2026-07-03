# TripoSG → ONNX export notes (feat/triposg-backend)

Research findings behind `scripts/export-triposg-onnx.py` — the exact inference
architecture of **TripoSG** (VAST-AI-Research/TripoSG, HF `VAST-AI/TripoSG`),
the tensor contract for the C++ ONNX Runtime side, the scheduler math the C++
loop must implement, and the license verification. All facts below were read
from the upstream sources on 2026-07-02:

- `triposg/pipelines/pipeline_triposg.py` (pipeline + `__call__` defaults)
- `triposg/schedulers/scheduling_rectified_flow.py` (`RectifiedFlowScheduler`)
- `triposg/models/transformers/triposg_transformer.py` (`TripoSGDiTModel`)
- `triposg/models/autoencoders/autoencoder_kl_triposg.py` (`TripoSGVAEModel`)
- `triposg/inference_utils.py` (`hierarchical_extract_geometry`)
- `scripts/inference_triposg.py`, `scripts/image_process.py` (reference driver)
- HF `VAST-AI/TripoSG`: `model_index.json` + per-component `config.json`s

---

## Pipeline overview (what actually runs at inference)

Unlike TripoSR (#764 — one feed-forward encoder/decoder pair), TripoSG is a
**rectified-flow diffusion** pipeline over a *vecset* latent (2048 tokens × 64
channels, no spatial layout):

```
input image
  │  background removal + white-bg composite + foreground crop      (host C++)
  │  BitImageProcessor: resize shortest-edge 256 → center-crop 224
  │                     → /255 → ImageNet mean/std normalize
  ▼
DINOv2-large (transformers Dinov2Model, 24 layers, hidden 1024)
  ▼  image_embeds = last_hidden_state  [1, 257, 1024]   (CLS token first)
latents x_T = N(0,1)  [1, 2048, 64]
  │  loop over N=50 timesteps (rectified flow, CFG scale 7.0):
  │      v = DiT(cat[x;x], t_i, cat[zeros; image_embeds])   ← doubled batch
  │      v = v_uncond + 7.0·(v_cond − v_uncond)
  │      x ← x + (σ_i − σ_{i+1})·v
  ▼
x_0 (denoised vecset latent)
  │  VAE decode: post_quant → 16 self-attn blocks over the 2048 tokens (ONCE)
  │              → per-query: frequency-embed(point) → 1 cross-attn block
  │              → norm_out → proj_out(1) → negate
  ▼
sdf(points)   INSIDE-POSITIVE, surface at 0
  │  upstream: skimage marching_cubes(grid, level=0) on a dense-then-refined
  │  octree grid over bounds (−1.005 … +1.005)³
  ▼
mesh (trimesh; exported as-is — no vertex transform / face flip in upstream)
```

There is **no CLIP branch** — DINOv2 is the only conditioning encoder
(`model_index.json` lists exactly: `feature_extractor_dinov2` /
`image_encoder_dinov2` / `scheduler` / `transformer` / `vae`).

---

## Verified component configs (verbatim from HF `VAST-AI/TripoSG`)

`transformer/config.json`:

```json
{ "_class_name": "TripoSGDiTModel", "cross_attention_dim": 1024,
  "in_channels": 64, "num_attention_heads": 16, "num_layers": 21, "width": 2048 }
```

`vae/config.json`:

```json
{ "_class_name": "Tripo2VAEModel", "embed_frequency": 8, "embed_include_pi": false,
  "embedding_type": "frequency", "in_channels": 3, "latent_channels": 64,
  "num_attention_heads": 8, "num_layers_decoder": 16, "num_layers_encoder": 8,
  "width_decoder": 1024, "width_encoder": 512 }
```

`scheduler/scheduler_config.json`:

```json
{ "_class_name": "RectifiedFlowScheduler", "num_train_timesteps": 1000,
  "shift": 1, "use_dynamic_shifting": false }
```

`image_encoder_dinov2/config.json`: `facebook/dinov2-large` — `hidden_size`
1024, `patch_size` 14, `num_hidden_layers` 24, `image_size` 518 (position table
base; interpolated down at runtime).

`feature_extractor_dinov2/preprocessor_config.json`: `BitImageProcessor`,
resize `shortest_edge: 256` (resample 3 = bicubic), `do_center_crop` to
**224×224**, rescale 1/255, normalize mean `[0.485,0.456,0.406]` /
std `[0.229,0.224,0.225]` (ImageNet). ⇒ DINOv2 runs at **224**, giving
`1 + (224/14)² = 257` tokens. (Contrast TripoSR: 512px, **no** mean/std.)

Checkpoint sizes (fp32 safetensors): DINOv2 1.22 GB, DiT **5.76 GB**
(~1.44 B params), VAE 0.97 GB. Total ~7.95 GB.

`__call__` defaults (pipeline_triposg.py, quoted):

```python
num_inference_steps=50, num_tokens=2048, guidance_scale=7.0,
bounds=(-1.005, -1.005, -1.005, 1.005, 1.005, 1.005),
dense_octree_depth=8, hierarchical_octree_depth=9, flash_octree_depth=9,
use_flash_decoder=True
```

(The reference driver `scripts/inference_triposg.py` calls
`pipe(image=img_pil, num_inference_steps=50, guidance_scale=7.0)`.)

---

## Tensor contract (what the exported graphs expose)

### a. `triposg_image_encoder.onnx` (~1.2 GB fp32)

| | name | dtype | shape | notes |
|--|------|-------|-------|-------|
| in  | `image`        | float32 | `[1, 3, 224, 224]` | RGB in `[0,1]`; ImageNet mean/std **baked into the graph** |
| out | `image_embeds` | float32 | `[1, 257, 1024]`   | `Dinov2Model(...).last_hidden_state`, CLS first |

C++ preprocessing to reproduce upstream (`scripts/image_process.py
prepare_image` + `BitImageProcessor`):

1. Matte the subject (upstream uses **BriaRMBG — non-commercial, do NOT use**;
   we substitute the existing Apache-2.0 U²-Net `BackgroundRemover`).
2. Composite over **WHITE** (`bg_color = [1.0, 1.0, 1.0]`) — note TripoSR uses
   gray 128; TripoSG's reference is white.
3. Crop to the foreground alpha bbox with **10% padding**
   (`padding_ratio=0.1` of the larger dimension, offset to center the smaller
   dimension — i.e. a roughly square, centered crop).
4. Resize shortest edge → 256 (bicubic), center-crop 224×224, divide by 255.
   (Normalization happens inside the graph.)

### b. `triposg_dit_step.onnx` (~5.7 GB fp32 → **external-data sidecar**; int8 ~1.5 GB single file)

| | name | dtype | shape | notes |
|--|------|-------|-------|-------|
| in  | `latents`      | float32 | `[B, 2048, 64]`  | B dynamic; 2 for CFG |
| in  | `timestep`     | float32 | `[B]`            | post-shift `t = 1000·σ_i` (same value repeated) |
| in  | `image_embeds` | float32 | `[B, 257, 1024]` | row order **[uncond; cond]**; uncond = all zeros |
| out | `velocity`     | float32 | `[B, 2048, 64]`  | model prediction ≈ `(x0 − noise)` |

Upstream CFG (quoted from the pipeline):

```python
latent_model_input = torch.cat([latents] * 2)
timestep = t.expand(latent_model_input.shape[0])
noise_pred = self.transformer(latent_model_input, timestep,
                              encoder_hidden_states=image_embeds, ...)[0]
noise_pred_uncond, noise_pred_image = noise_pred.chunk(2)
noise_pred = noise_pred_uncond + self.guidance_scale * (noise_pred_image - noise_pred_uncond)
```

`encode_image` builds the uncond row as `torch.zeros_like(image_embeds)` and
concatenates `[negative, positive]` — so the C++ side can either run one B=2
call (upstream-exact) or two B=1 calls (identical math; the export's
`--verify` cross-checks B=1 vs B=2 row 0).

**>2 GB note:** the fp32 DiT exceeds the 2 GB protobuf limit, so it ships as
`triposg_dit_step.onnx` **plus** `triposg_dit_step.onnx.data` (single
consolidated external-data file, same directory). ONNX Runtime loads it
transparently, but the C++ downloader must fetch **both** files and keep them
side by side. The int8 variant is a single file.

### c. `triposg_vae_latents.onnx` (~0.9 GB fp32 — run ONCE per generation)

| | name | dtype | shape |
|--|------|-------|-------|
| in  | `latents`  | float32 | `[1, 2048, 64]` (the denoised x₀) |
| out | `kv_cache` | float32 | `[1, 2048, 1024]` |

This is `post_quant` (Linear 64→1024) + the `TripoSGDecoder`'s 16
self-attention blocks over the latent set — upstream's `kv_cache` (computed on
the first `_decode` chunk, reused for all later chunks; type-hinted
`Optional[torch.Tensor]`, i.e. one tensor, not per-layer pairs).

### d. `triposg_vae_decoder.onnx` (small — per-chunk query graph)

| | name | dtype | shape | notes |
|--|------|-------|-------|-------|
| in  | `kv_cache` | float32 | `[1, 2048, 1024]` | from graph (c) |
| in  | `points`   | float32 | `[1, P, 3]` | **P dynamic**; world coords within `(−1.005 … +1.005)` |
| out | `sdf`      | float32 | `[1, P, 1]` | **inside-positive**, surface at iso **0.0** |

Internals: `FrequencyPositionalEmbedding(num_freqs=8, logspace=True,
input_dim=3, include_pi=False)` (3 → 3 + 3·2·8 = 51 channels) → query
projection → **one** cross-attention DiT block against `kv_cache` → `norm_out`
→ `proj_out` (Linear → 1) → `* −1`.

`--monolithic` additionally emits `triposg_vae_decode_mono.onnx`
(`latents [1,2048,64] + points [1,P,3] → sdf [1,P,1]`) — the unsplit reference
used to validate the kv-split (the script always torch-checks the split
against `vae.decode(latents, sampled_points=pts).sample` and warns loudly on
mismatch).

---

## Scheduler math (the C++ loop contract)

`RectifiedFlowScheduler` (released config: `num_train_timesteps=1000`,
`shift=1`, `use_dynamic_shifting=false`). Quoted upstream `set_timesteps`:

```python
timesteps = np.array([(1.0 - i / num_inference_steps) * 1000 for i in range(num_inference_steps)])
sigmas = timesteps / 1000
sigmas = shift * sigmas / (1 + (shift - 1) * sigmas)   # identity when shift == 1
timesteps = sigmas * 1000
self.sigmas = cat([sigmas, zeros(1)])                  # σ_N = 0 appended
```

and `step`:

```python
sigma      = self.sigmas[i]
sigma_next = self.sigmas[i + 1]
prev_sample = sample + (sigma - sigma_next) * model_output
```

So for **N = 50** (default): `σ_i = 1 − i/50` → `1.00, 0.98, …, 0.02`, plus
`σ_50 = 0`; `t_i = 1000·σ_i` → `1000, 980, …, 20`; every step is
`x ← x + 0.02 · v_cfg` (uniform Δσ = 1/N). Initial latents are **pure
N(0,1)** — no `init_noise_sigma` scaling exists, consistent with the training
interpolation `x_t = (1−σ)·x0 + σ·noise` (`scale_noise`, quoted), which at
σ=1 is pure noise. Hence the model output points from noise toward data
(≈ `x0 − ε`), and the `+(σ_i − σ_{i+1})·v` update (σ decreasing) integrates to
x₀ at σ=0.

⚠ **Sign trap:** diffusers' stock `FlowMatchEulerDiscreteScheduler` writes the
update as `sample + (sigma_next − sigma) * model_output` (its models predict
`ε − x0`). TripoSG's scheduler/model use the **opposite** convention. Implement
exactly the formula above, not the diffusers one.

C++ pseudo-loop:

```cpp
latents = randn(1, 2048, 64);                    // fixed seed for reproducibility
for (i = 0; i < N; ++i) {
    float t = 1000.f * sigma[i];
    v_u = dit(latents, t, zeros_embeds);         // or one B=2 call
    v_c = dit(latents, t, image_embeds);
    v   = v_u + guidance * (v_c - v_u);          // guidance = 7.0
    latents += (sigma[i] - sigma[i + 1]) * v;
}
```

CFG is applied when `guidance_scale > 1` (diffusers convention); the reference
driver always uses 7.0.

---

## Surface extraction (host-side marching cubes)

Upstream defaults to a "flash" octree decoder and offers
`hierarchical_extract_geometry` — both are torch/GPU octree machinery we do
**not** port. The dense path we replicate (from `hierarchical_extract_geometry`):

- Grid: `linspace(bbox_min, bbox_max, num_cells)` per axis over bounds
  `(−1.005, +1.005)` with `dense_octree_depth=8` (≈ 2⁸(+1) = 256/257 samples
  per axis; upstream then refines near-surface cells to depth 9 ≈ 512).
  For our C++ dense-grid approach: query `resolution³` points, resolution
  user-tunable (256 default / 128 preview — matching the TripoSR tiers).
- Field: `grid_logits` from `vae.decode(...).sample` used **directly** —
  `skimage.measure.marching_cubes(grid_logits, level=0)` with the default
  `gradient_direction="descent"` (object = values **greater** than the level).
  I.e. the exported `sdf` output is **inside-positive with the surface at 0** —
  the same convention as TripoSR's `density − threshold`, so
  `MarchingCubes::extract(field = sdf, isoLevel = 0)` drops straight in.
- Winding: upstream applies no face flip after skimage MC. Our native MC
  emitted flipped winding (`v0,v2,v1`) for TripoSR's inside-positive field —
  the same flip should apply here, but **verify empirically on the first
  end-to-end run** (render both windings once).
- Vertex rescale (upstream): `verts / 2^depth * bbox_size + bbox_min`. With our
  MC's `gridMin/gridMax = ±1.005` this is handled by the existing world-box
  mapping.
- No threshold constant (unlike TripoSR's 25.0) — the level is plain 0.0
  (`mc_level=0.0` default in `flash_extract_geometry` too).

---

## Licenses (runtime components) — verified 2026-07-02

| Component | Source | License | OK? |
|-----------|--------|---------|-----|
| TripoSG code | github.com/VAST-AI-Research/TripoSG `LICENSE` | **MIT** ("Copyright (c) 2025 VAST-AI-Research and contributors") | ✅ |
| TripoSG weights (DiT + VAE) | HF `VAST-AI/TripoSG` | **MIT** (repo license tag: mit) | ✅ |
| DINOv2-large weights | redistributed inside HF `VAST-AI/TripoSG` (`image_encoder_dinov2/`); upstream `facebook/dinov2-large` | **Apache-2.0** upstream; shipped copy sits in the MIT-tagged repo | ✅ (credit Meta AI in THIRD_PARTY_AI_MODELS.md) |
| Background removal | upstream uses **briaai/RMBG-1.4** (`scripts/briarmbg.py`) | **NON-COMMERCIAL** (bria-rmbg-1.4: commercial use requires a paid BRIA agreement) | ❌ **DO NOT SHIP** — substitute our existing U²-Net (Apache-2.0) `BackgroundRemover`, already in the app for TripoSR |
| skimage / torchmcubes MC | upstream extraction | BSD / MIT | N/A — replaced by our native `src/MarchingCubes` |
| ONNX Runtime | existing dependency | MIT | ✅ |

The only license landmine is **BriaRMBG** — it is quarantined to the upstream
repo's demo scripts and never touches the export or the C++ runtime path.

---

## Export mechanics & risks

- **Exporter:** legacy TorchScript path (`dynamo=False`), opset **18**,
  `do_constant_folding=True` — repo convention (TripoSR used opset 17; 18 adds
  nothing risky and is ORT 1.20.1-supported).
- **DINOv2 position-embedding interpolation:** same trap as TripoSR's ViT —
  `interpolate_pos_encoding` calls `F.interpolate(bicubic, antialias)` to map
  the 518-base table to the 224 input. Fixed input ⇒ the table is a constant;
  the script precomputes it and monkeypatches the method
  (`freeze_dinov2_pos_encoding`). `# VERIFY:` signature checked against
  transformers 4.45.x (the version the HF configs were saved with).
- **>2 GB DiT:** exported via a scratch dir, then consolidated with
  `onnx.save_model(save_as_external_data=True, all_tensors_to_one_file=True)`
  into `triposg_dit_step.onnx` + `triposg_dit_step.onnx.data`. Hosting and the
  C++ downloader must treat the pair atomically. The int8 variant
  (MatMul-only dynamic QInt8 — avoids ConvInteger, the TripoSR lesson) is a
  single ~1.5 GB file and is the realistic default download tier.
- **VAE kv-split (`# VERIFY:` in the script):** `TripoSGDecoder.forward(sample,
  queries, kv_cache)` computes `kv_cache` from `sample` via `blocks[:-1]` when
  `None`, and the query path (`proj_query` → final cross-attn block →
  `norm_out`/`proj_out` → `*−1`) reads only the cache. The split wrappers rely
  on (1) the cache being a single tensor and (2) `sample` being unused when the
  cache is provided. The script *always* cross-checks the split against
  `vae.decode(latents, sampled_points=pts).sample` in torch and warns loudly;
  `--monolithic` provides the unsplit fallback graph.
- **Monolithic trace trick:** `_decode`'s python chunk loop is traced with
  `num_chunks = 1<<40` so it unrolls to exactly one iteration and the traced
  slice stays valid for any dynamic `P`.
- **Chunk independence:** upstream chunks queries arbitrarily (50 000/chunk)
  and concatenates, so per-chunk results are independent by construction — the
  dynamic-P graph is exact for any C++ chunk size (verified in `--verify` with
  P=777 vs P=4096 prefixes).
- **RoPE / QK-norm / U-Net skips:** `TripoSGDiTModel` uses RMS QK-norm, U-Net
  style skip connections between its 21 blocks, and threads an optional
  `image_rotary_emb` argument (unused by this pipeline — nothing computes it in
  `__call__`). All are ordinary traceable ops; the time embedding
  (`Timesteps` sinusoid + GELU MLP, prepended as an extra sequence token and
  stripped before `proj_out`) is inside the graph, so C++ passes the plain
  float timestep.
- **Compute reality check:** a 1.44 B-param DiT × 2 (CFG) × 50 steps on CPU is
  minutes-scale, not seconds — expect ~10–60 min fp32 on a laptop CPU; int8 +
  fewer steps (rectified flow degrades gracefully at e.g. N=25) + CoreML EP are
  the practical knobs. This is the main product risk vs TripoSR, not a
  feasibility risk.
- **Not fetched/verified (low risk, flagged):** the exact
  `generate_dense_grid_points_gpu` cell count (2⁸ vs 2⁸+1 samples per axis —
  irrelevant to our own grid choice), the upstream repo `requirements.txt`
  pins (the script documents `diffusers==0.30.3` from the checkpoint metadata
  and `transformers>=4.45`), and the `TripoSGDecoder` internal attribute names
  (not needed — the wrappers call whole modules, never sub-attributes).

---

## Live-test findings (2026-07-03, first end-to-end C++ runs)

- **Per-tensor int8 destroys the geometry.** The shipped
  `triposg_dit_step_int8.onnx` (MatMul-only dynamic QInt8, per-tensor) has
  max-rel-err 1.67e-02 on a single step, but the error COMPOUNDS over the flow
  loop (2 CFG calls/step, amplified 7× by guidance): 4 steps → blobby but
  semi-coherent, 8 steps → fragmented, 25 steps → disconnected noise splatter.
  The fp32 DiT with the identical C++ loop produces a coherent single figure —
  the loop/export/decode are correct; the quantization is the culprit.
  `export-triposg-onnx.py` now quantizes `per_channel=True, reduce_range=True`;
  the hosted int8 file must be re-exported + re-uploaded before the int8 tier
  is trustworthy. Until then fp32 is the only recommended tier.
- **Decoder chunk must stay small (8192).** The VAE decoder cross-attends every
  query point to the 2048 kv tokens, so per-Run activation memory is linear in
  P with a huge constant: TripoSR's 262144-point chunk transiently allocated
  ~90 GB at res 256 and macOS killed the process. `TripoSGPredictor` hard-caps
  the chunk at 8192 (a few hundred MB per Run).
- **Sessions are staged, not concurrent.** Encoder (~1.1 GB), DiT (1.3–5.4 GB)
  and vae_latents (~769 MB) are each opened when their stage starts and
  destroyed when it completes; only the ~48 MB point decoder lives through the
  long decode/refine tail. Peak RSS ≈ the largest single stage (measured
  ~1.1 GB int8 end-to-end), instead of the >4 GB sum that previously tripped
  memory pressure alongside the 90 GB chunk bug.
- **Image preprocessing matches BitImageProcessor now:** resize shortest edge
  to 8/7·crop (256 for 224) + centre-crop, not a squash-resize.
