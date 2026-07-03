#!/usr/bin/env python3
"""Export TripoSG (image → 3D shape diffusion) to ONNX graphs for the C++ backend.

ONE-TIME, OFFLINE developer tool — NOT shipped with the app, NOT wired into CMake
or CI. The app never runs Python: it runs the exported .onnx files in C++ via
ONNX Runtime (CPU / CoreML EP), downloading them on first use.

MODEL: TripoSG (VAST-AI-Research/TripoSG — "TripoSG: High-Fidelity 3D Shape
  Synthesis using Large-Scale Rectified Flow Models", arXiv 2502.06608).
  Code **MIT** (repo LICENSE: "MIT License, Copyright (c) 2025 VAST-AI-Research
  and contributors"); weights **MIT** (HF `VAST-AI/TripoSG`, license tag: mit).
  The bundled image encoder is facebook/dinov2-large (Apache-2.0 upstream,
  redistributed inside the MIT-tagged HF repo). Clears QtMeshEditor's
  permissive-redistribution bar — see THIRD_PARTY_AI_MODELS.md and
  docs/TRIPOSG_EXPORT_NOTES.md.
  ⚠ The upstream repo's background-removal helper (scripts/briarmbg.py,
  briaai/RMBG-1.4) is NON-COMMERCIAL and must NOT be shipped or exported;
  the app substitutes its existing Apache-2.0 U²-Net BackgroundRemover.

WHY FOUR GRAPHS (unlike TripoSR's clean encoder/decoder pair)
  TripoSG is a *diffusion* pipeline — a rectified-flow DiT denoises a 2048-token
  vecset latent over N steps, conditioned on DINOv2 image features via
  cross-attention, then a vecset VAE decodes SDF values at query points:

      image ── DINOv2-large ──────────────► image_embeds [1,257,1024]   (graph a)
      latents[1,2048,64] = N(0,1)
      loop i = 0..N-1 (C++ owns the scheduler loop):
          v = DiT(latents, t_i, embeds)     velocity prediction          (graph b)
          v = v_u + s·(v_c − v_u)           classifier-free guidance (C++)
          latents += (σ_i − σ_{i+1})·v      rectified-flow Euler step (C++)
      kv = VAE latent processing(latents)   run ONCE                     (graph c)
      sdf(points) = VAE query(kv, points)   chunked over the grid        (graph d)
      mesh = marching cubes on sdf at iso 0 (inside-positive)            (host C++)

  The heavy VAE self-attention stack over the 2048 latent tokens must not be
  re-run for every grid chunk (a 257³ grid is ~340 chunks of 50k points), so the
  VAE decode is split into a run-once "latents" graph (c) and a tiny per-chunk
  "query" graph (d) — mirroring upstream's kv_cache. `--monolithic` additionally
  emits the unsplit (latents, points) → sdf graph as a correctness reference.

WHAT IT PRODUCES  (the contract the C++ predictor targets; full details +
scheduler math in docs/TRIPOSG_EXPORT_NOTES.md)

  triposg_image_encoder.onnx      (~1.2 GB fp32 — DINOv2-large, ImageNet
                                   normalization baked into the graph)
    input  "image"        float32 [1, 3, 224, 224]   RGB in [0,1]
    output "image_embeds" float32 [1, 257, 1024]     last_hidden_state (CLS first)

  triposg_dit_step.onnx           (~5.7 GB fp32 → external-data sidecar
                                   triposg_dit_step.onnx.data; int8 ~1.5 GB single file)
    input  "latents"      float32 [B, 2048, 64]      B dynamic (2 for CFG: [uncond;cond])
    input  "timestep"     float32 [B]                post-shift t = 1000·σ_i
    input  "image_embeds" float32 [B, 257, 1024]     row 0 = zeros (uncond), row 1 = cond
    output "velocity"     float32 [B, 2048, 64]      model_output ≈ (x0 − noise)

  triposg_vae_latents.onnx        (~0.9 GB fp32 — post_quant + 16 self-attn blocks)
    input  "latents"      float32 [1, 2048, 64]      the denoised x0
    output "kv_cache"     float32 [1, 2048, 1024]    processed latent set (run once)

  triposg_vae_decoder.onnx        (small — frequency embed + 1 cross-attn block + head)
    input  "kv_cache"     float32 [1, 2048, 1024]
    input  "points"       float32 [1, P, 3]          P dynamic; world coords in
                                                     bounds (−1.005 … +1.005)³
    output "sdf"          float32 [1, P, 1]          INSIDE-POSITIVE, surface at 0.0
                                                     (upstream negates its raw logits;
                                                      this output == vae.decode().sample)

  + triposg_dit_step_int8.onnx    (unless --no-quant; MatMul-only dynamic QInt8)
  + triposg_vae_decode_mono.onnx  (only with --monolithic; (latents, points) → sdf)

SAMPLING DEFAULTS (upstream pipeline_triposg.py __call__):
  num_inference_steps=50, guidance_scale=7.0, num_tokens=2048,
  scheduler = RectifiedFlowScheduler(num_train_timesteps=1000, shift=1)
  σ_i = 1 − i/N (shift=1 ⇒ identity shift), σ_N = 0; t_i = 1000·σ_i
  step: latents ← latents + (σ_i − σ_{i+1}) · velocity     [uniform 0.02 at N=50]
  decode bounds (−1.005…1.005)³, dense grid depth 8 (≈257³), MC at iso 0.

USAGE (offline venv; ~8 GB download from HF `VAST-AI/TripoSG` on first run):
    python3 -m venv venv
    ./venv/bin/pip install torch --index-url https://download.pytorch.org/whl/cpu
    ./venv/bin/pip install "diffusers==0.30.3" "transformers>=4.45,<5" onnx \
        onnxruntime huggingface_hub numpy einops jaxtyping safetensors accelerate
    git clone --depth 1 https://github.com/VAST-AI-Research/TripoSG
    ./venv/bin/python scripts/export-triposg-onnx.py \
        --triposg ./TripoSG --out dist/triposg_onnx --verify

Then upload the .onnx (+ the DiT's .onnx.data sidecar) to the HF models repo
under triposg/ (fernandotonon/QtMeshEditor-models) — the TripoSR precedent.
"""
import argparse
import os
import shutil
import sys
import tempfile

import numpy as np
import torch
import torch.nn as nn

# ---------------------------------------------------------------------------
# Constants measured from the released VAST-AI/TripoSG checkpoint configs
# (see docs/TRIPOSG_EXPORT_NOTES.md for the verbatim config.json contents).
# ---------------------------------------------------------------------------
NUM_TOKENS = 2048          # pipeline __call__ default num_tokens
LATENT_CHANNELS = 64       # vae/config.json latent_channels == transformer in_channels
COND_TOKENS = 257          # DINOv2-large @224: 1 CLS + (224/14)^2 = 257
COND_DIM = 1024            # dinov2-large hidden_size == transformer cross_attention_dim
IMAGE_SIZE = 224           # BitImageProcessor: resize shortest edge 256 -> center crop 224
VAE_WIDTH_DECODER = 1024   # vae/config.json width_decoder (kv_cache channel dim)
IMAGENET_MEAN = [0.485, 0.456, 0.406]
IMAGENET_STD = [0.229, 0.224, 0.225]
DEFAULT_STEPS = 50
DEFAULT_GUIDANCE = 7.0
DECODE_BOUNDS = 1.005      # pipeline __call__ default bounds (±1.005 per axis)

TWO_GB = 2 * 1024 * 1024 * 1024


def log(tag, msg):
    print(f"[{tag}] {msg}", flush=True)


# ---------------------------------------------------------------------------
# Model loading
# ---------------------------------------------------------------------------
def load_pipeline(triposg_dir, weights_dir):
    """Import the triposg package from a git checkout and build the diffusers
    pipeline from the HF weights. Returns the TripoSGPipeline on CPU/fp32."""
    sys.path.insert(0, os.path.abspath(triposg_dir))

    # `triposg.inference_utils` imports `from diso import DiffDMC` at module
    # scope. diso is a CUDA-only differentiable-marching-cubes package that
    # neither installs on macOS/CPU boxes nor matters here: this export only
    # touches the pipeline's MODELS (image encoder / DiT / VAE); the app does
    # surface extraction with its own native marching cubes. Stub it out —
    # the exact torchmcubes trick the TripoSR exporter uses.
    import types  # noqa: E402
    if "diso" not in sys.modules:
        _diso = types.ModuleType("diso")
        class _DiffDMCStub:  # noqa: N801 — never instantiated by this export
            def __init__(self, *a, **k):
                raise RuntimeError("diso stub: not available in this export env")
        _diso.DiffDMC = _DiffDMCStub
        sys.modules["diso"] = _diso
        log("note", "stubbed CUDA-only 'diso' (unused by the export)")

    from triposg.pipelines.pipeline_triposg import TripoSGPipeline  # noqa: E402

    if weights_dir is None:
        from huggingface_hub import snapshot_download
        weights_dir = snapshot_download("VAST-AI/TripoSG")
        log("ok", f"downloaded VAST-AI/TripoSG -> {weights_dir}")

    pipe = TripoSGPipeline.from_pretrained(weights_dir)
    pipe = pipe.to("cpu", torch.float32)
    for m in (pipe.image_encoder_dinov2, pipe.transformer, pipe.vae):
        m.eval()
    return pipe


def freeze_dinov2_pos_encoding(dinov2, image_size):
    """DINOv2's interpolate_pos_encoding resizes its position table (built for
    image_size=518, patch 14) to the runtime grid via F.interpolate(bicubic,
    antialias) — the same ViT-at-non-native-resolution construct that broke the
    TripoSR export trace. The ONNX input size is FIXED (224), so the
    interpolated table is a constant: precompute it once in eager mode and
    monkeypatch interpolate_pos_encoding to return the frozen tensor.

    # VERIFY: transformers' Dinov2Embeddings.interpolate_pos_encoding(embeddings,
    # height, width) only reads embeddings.shape — validated against
    # transformers 4.45.x; if a future version changes the signature the
    # try/except below falls through to a plain trace (which may or may not
    # export depending on the interpolate call)."""
    try:
        emb = dinov2.embeddings
        num_patches = (image_size // dinov2.config.patch_size) ** 2
        with torch.no_grad():
            dummy = torch.zeros(1, 1 + num_patches, dinov2.config.hidden_size)
            frozen = emb.interpolate_pos_encoding(dummy, image_size, image_size).detach()
        emb.interpolate_pos_encoding = lambda embeddings, height, width: frozen
        log("ok", f"froze DINOv2 pos-encoding at {image_size}px "
                  f"(table {tuple(frozen.shape)})")
    except Exception as e:  # noqa: BLE001
        log("warn", f"could not freeze DINOv2 pos-encoding ({e}); "
                    f"attempting plain trace")


# ---------------------------------------------------------------------------
# Export wrappers
# ---------------------------------------------------------------------------
class ImageEncoderWrapper(nn.Module):
    """image [B,3,224,224] RGB in [0,1] -> image_embeds [B,257,1024].

    Reproduces pipeline.encode_image minus the PIL BitImageProcessor: the C++
    side composites the U²-Net-matted subject over WHITE, crops to the
    foreground bbox (+10% padding, centered square-ish — upstream
    scripts/image_process.py prepare_image), resizes shortest-edge-256
    (bicubic) + center-crops 224, and feeds /255 RGB. The ImageNet mean/std
    normalization (BitImageProcessor do_normalize) is baked in HERE so the C++
    preprocessing stays a plain [0,1] resize like TripoSR's."""

    def __init__(self, dinov2):
        super().__init__()
        self.dinov2 = dinov2
        self.register_buffer("mean", torch.tensor(IMAGENET_MEAN).view(1, 3, 1, 1))
        self.register_buffer("std", torch.tensor(IMAGENET_STD).view(1, 3, 1, 1))

    def forward(self, image):
        x = (image - self.mean) / self.std
        # pipeline_triposg.encode_image: image_encoder_dinov2(image).last_hidden_state
        return self.dinov2(x).last_hidden_state


class DiTStepWrapper(nn.Module):
    """One denoising step: (latents, timestep, image_embeds) -> velocity.

    Mirrors the pipeline's transformer call exactly:
        noise_pred = self.transformer(latent_model_input, timestep,
                                      encoder_hidden_states=image_embeds,
                                      return_dict=False)[0]
    The C++ side owns the loop, the CFG combine and the Euler update (see the
    module docstring / docs/TRIPOSG_EXPORT_NOTES.md). Batch axis is dynamic so
    the caller can run the doubled [uncond; cond] batch in one call (upstream
    behavior) or two B=1 calls — mathematically identical since the uncond
    embeddings are all-zeros (encode_image: torch.zeros_like(image_embeds))."""

    def __init__(self, transformer):
        super().__init__()
        self.transformer = transformer

    def forward(self, latents, timestep, image_embeds):
        return self.transformer(
            latents, timestep,
            encoder_hidden_states=image_embeds,
            return_dict=False,
        )[0]


class VaeLatentsWrapper(nn.Module):
    """latents [1,2048,64] -> kv_cache [1,2048,1024]. Run ONCE per generation.

    Reproduces the first-chunk half of TripoSGVAEModel._decode: post_quant(z),
    then TripoSGDecoder processes the latent set through its self-attention
    blocks and returns the cache (decoder.forward returns (logits, kv_cache);
    the kv_cache is the processed latent features the final cross-attention
    block attends to). We obtain it by running decoder.forward with a single
    throwaway query point.

    # VERIFY: TripoSGDecoder.forward(sample, queries, kv_cache) type-hints
    # kv_cache as Optional[torch.Tensor] (a single tensor, not a per-layer
    # list) and computes it as blocks[:-1] applied to `sample` when None —
    # confirmed from the upstream source read (docs/TRIPOSG_EXPORT_NOTES.md
    # §VAE). If the returned cache is NOT a plain [1,2048,1024] tensor, the
    # printed contract below will show it and the C++ contract must follow."""

    def __init__(self, vae):
        super().__init__()
        self.vae = vae

    def forward(self, latents):
        z = self.vae.post_quant(latents)
        dummy_q = self.vae.embedder(torch.zeros(1, 1, 3, dtype=latents.dtype))
        _, kv_cache = self.vae.decoder(z, dummy_q, None)
        return kv_cache


class VaeQueryWrapper(nn.Module):
    """(kv_cache [1,2048,1024], points [1,P,3]) -> sdf [1,P,1]. Chunked by C++.

    Reproduces the per-chunk half of _decode: frequency-embed the query points
    (FrequencyPositionalEmbedding num_freqs=8, logspace, include_pi=False:
    3 -> 3+3·2·8 = 51 channels), then decoder.forward with the precomputed
    kv_cache (which skips the latent self-attention stack). decoder.forward
    already negates its raw logits (`logits * -1`), so this output equals
    vae.decode(...).sample: INSIDE-POSITIVE, surface at iso 0.

    # VERIFY: when kv_cache is not None, decoder.forward must not read
    # `sample` (the upstream first-chunk/kv_cache dance implies it, and we
    # pass the kv_cache tensor itself as `sample` so any residual read sees
    # sane shapes). The --verify mode cross-checks this graph against
    # vae.decode(latents, sampled_points=pts).sample — a mismatch there means
    # this assumption broke."""

    def __init__(self, vae):
        super().__init__()
        self.vae = vae

    def forward(self, kv_cache, points):
        queries = self.vae.embedder(points)
        logits, _ = self.vae.decoder(kv_cache, queries, kv_cache)
        return logits


class VaeDecodeMonolithic(nn.Module):
    """(latents [1,2048,64], points [1,P,3]) -> sdf [1,P,1] — the unsplit
    reference graph (recomputes the latent stack every call; correctness
    baseline for --verify and a fallback if the kv_cache split misbehaves).

    num_chunks is set beyond any realistic P so _decode's python chunking loop
    runs exactly ONE iteration during tracing — the traced slice
    xyz_samples[:, 0:huge] stays valid for any dynamic P at runtime."""

    def __init__(self, vae):
        super().__init__()
        self.vae = vae

    def forward(self, latents, points):
        return self.vae._decode(latents, points,
                                num_chunks=1 << 40, return_dict=False)[0]


# ---------------------------------------------------------------------------
# Export helpers
# ---------------------------------------------------------------------------
def export_onnx(module, args_tuple, path, input_names, output_names,
                dynamic_axes, opset):
    """torch.onnx.export (legacy TorchScript exporter, dynamo=False — the repo
    convention; no onnxscript dependency) with >2GB external-data handling:
    export into a scratch dir (torch scatters per-tensor external files for
    >2GB graphs), then consolidate into <name>.onnx + single <name>.onnx.data."""
    import onnx

    out_dir = os.path.dirname(os.path.abspath(path))
    base = os.path.basename(path)
    scratch = tempfile.mkdtemp(prefix="triposg_export_", dir=out_dir)
    scratch_path = os.path.join(scratch, base)
    try:
        torch.onnx.export(
            module, args_tuple, scratch_path,
            input_names=input_names, output_names=output_names,
            dynamic_axes=dynamic_axes,
            opset_version=opset, do_constant_folding=True,
            dynamo=False,
        )
        # Consolidate: single .onnx (+ single .onnx.data only when >2GB).
        model = onnx.load(scratch_path, load_external_data=True)
        total = sum(os.path.getsize(os.path.join(scratch, f))
                    for f in os.listdir(scratch))
        if total >= TWO_GB:
            data_name = base + ".data"
            onnx.save_model(model, path, save_as_external_data=True,
                            all_tensors_to_one_file=True, location=data_name,
                            convert_attribute=False)
            log("ok", f"wrote {path} (+ external data sidecar {data_name}, "
                      f"total ~{total / 1e9:.2f} GB) — host BOTH files together")
        else:
            onnx.save_model(model, path)
            log("ok", f"wrote {path} (~{total / 1e9:.2f} GB)")
    finally:
        shutil.rmtree(scratch, ignore_errors=True)


def print_scheduler_contract(num_steps):
    """Print the exact rectified-flow schedule the C++ loop must implement
    (RectifiedFlowScheduler with the released config: num_train_timesteps=1000,
    shift=1, use_dynamic_shifting=False)."""
    shift = 1.0
    sig = np.array([(1.0 - i / num_steps) for i in range(num_steps)])
    sig = shift * sig / (1 + (shift - 1) * sig)   # identity at shift=1
    sigmas = np.concatenate([sig, [0.0]])
    timesteps = sig * 1000.0
    log("contract", f"scheduler: N={num_steps} shift={shift}")
    log("contract", f"  sigmas[0..3]={np.round(sigmas[:4], 4).tolist()} ... "
                    f"sigmas[-3:]={np.round(sigmas[-3:], 4).tolist()}")
    log("contract", f"  timesteps[0..3]={np.round(timesteps[:4], 1).tolist()} ... "
                    f"timesteps[-2:]={np.round(timesteps[-2:], 1).tolist()}")
    log("contract", "  init:  latents = N(0,1) [1,2048,64]")
    log("contract", "  step:  latents += (sigma[i] - sigma[i+1]) * "
                    "(v_uncond + guidance * (v_cond - v_uncond))")
    log("contract", f"  defaults: guidance={DEFAULT_GUIDANCE}, "
                    f"CFG batch order = [uncond(zeros embeds); cond]")
    return sigmas, timesteps


# ---------------------------------------------------------------------------
def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    ap.add_argument("--triposg", required=True,
                    help="path to a VAST-AI-Research/TripoSG git checkout")
    ap.add_argument("--weights", default=None,
                    help="local VAST-AI/TripoSG weights dir (default: "
                         "snapshot_download from HF)")
    ap.add_argument("--out", default="./out", help="output dir for the .onnx files")
    ap.add_argument("--opset", type=int, default=18)
    ap.add_argument("--steps", type=int, default=DEFAULT_STEPS,
                    help="inference steps to print the reference sigma schedule for")
    ap.add_argument("--verify", action="store_true",
                    help="run onnxruntime round-trip checks against torch")
    ap.add_argument("--no-quant", action="store_true",
                    help="skip the int8 DiT variant (export fp32 only)")
    ap.add_argument("--monolithic", action="store_true",
                    help="also export the unsplit (latents, points)->sdf VAE graph")
    ap.add_argument("--skip-image-encoder", action="store_true",
                    help="skip the DINOv2 graph (iterate on DiT/VAE only)")
    args = ap.parse_args()

    os.makedirs(args.out, exist_ok=True)
    torch.manual_seed(0)

    pipe = load_pipeline(args.triposg, args.weights)
    tcfg = pipe.transformer.config
    vcfg = pipe.vae.config
    log("contract", f"transformer: in_channels={tcfg.in_channels} "
                    f"width={tcfg.width} layers={tcfg.num_layers} "
                    f"cross_attention_dim={tcfg.cross_attention_dim}")
    log("contract", f"vae: latent_channels={vcfg.latent_channels} "
                    f"width_decoder={vcfg.width_decoder} "
                    f"num_layers_decoder={vcfg.num_layers_decoder} "
                    f"embed_frequency={vcfg.embed_frequency}")
    assert tcfg.in_channels == LATENT_CHANNELS
    assert vcfg.latent_channels == LATENT_CHANNELS

    print_scheduler_contract(args.steps)
    log("contract", f"decode bounds=(-{DECODE_BOUNDS}..+{DECODE_BOUNDS})^3, "
                    f"dense grid depth 8 (~257^3), sdf INSIDE-POSITIVE, MC iso 0.0")

    # ---- (a) image encoder ---------------------------------------------------
    enc_path = os.path.join(args.out, "triposg_image_encoder.onnx")
    dummy_img = torch.rand(1, 3, IMAGE_SIZE, IMAGE_SIZE, dtype=torch.float32)
    if not args.skip_image_encoder:
        freeze_dinov2_pos_encoding(pipe.image_encoder_dinov2, IMAGE_SIZE)
        enc = ImageEncoderWrapper(pipe.image_encoder_dinov2).eval()
        with torch.no_grad():
            embeds_ref = enc(dummy_img)
        log("contract", f"image_embeds shape={tuple(embeds_ref.shape)} "
                        f"(expect [1,{COND_TOKENS},{COND_DIM}])")
        export_onnx(enc, (dummy_img,), enc_path,
                    ["image"], ["image_embeds"],
                    {"image": {0: "B"}, "image_embeds": {0: "B"}},
                    args.opset)
    else:
        with torch.no_grad():
            embeds_ref = ImageEncoderWrapper(pipe.image_encoder_dinov2)(dummy_img)

    # ---- (b) DiT denoising step ----------------------------------------------
    dit = DiTStepWrapper(pipe.transformer).eval()
    B = 2  # trace with the CFG-doubled batch (upstream shape)
    dummy_lat = torch.randn(B, NUM_TOKENS, LATENT_CHANNELS, dtype=torch.float32)
    dummy_t = torch.full((B,), 1000.0, dtype=torch.float32)
    dummy_cond = torch.cat([torch.zeros_like(embeds_ref), embeds_ref], dim=0)
    with torch.no_grad():
        v_ref = dit(dummy_lat, dummy_t, dummy_cond)
    log("contract", f"velocity shape={tuple(v_ref.shape)} "
                    f"(expect [{B},{NUM_TOKENS},{LATENT_CHANNELS}])")

    dit_path = os.path.join(args.out, "triposg_dit_step.onnx")
    export_onnx(dit, (dummy_lat, dummy_t, dummy_cond), dit_path,
                ["latents", "timestep", "image_embeds"], ["velocity"],
                {"latents": {0: "B"}, "timestep": {0: "B"},
                 "image_embeds": {0: "B"}, "velocity": {0: "B"}},
                args.opset)

    # ---- int8 tier of the heaviest graph (the ~5.7 GB DiT) --------------------
    # MatMul-only dynamic QInt8 (repo precedent from export-triposr-onnx.py:
    # quantizing Conv would emit ConvInteger which our ORT CPU EP lacks; the
    # DiT is MatMul-dominated so it still shrinks ~4x to a single-file model).
    if not args.no_quant:
        try:
            from onnxruntime.quantization import quantize_dynamic, QuantType
            int8_path = os.path.join(args.out, "triposg_dit_step_int8.onnx")
            quantize_dynamic(dit_path, int8_path, weight_type=QuantType.QInt8,
                             op_types_to_quantize=["MatMul"],
                             use_external_data_format=False)
            log("ok", f"wrote {int8_path}")
        except Exception as e:  # noqa: BLE001 — best-effort; fp32 still ships
            log("warn", f"int8 DiT export skipped: {e}")
            log("warn", "  (if the failure is the >2GB input, retry with "
                        "onnxruntime>=1.17 which reads external-data models)")

    # ---- (c) VAE latent processing (run once) ---------------------------------
    vae_lat = VaeLatentsWrapper(pipe.vae).eval()
    lat1 = torch.randn(1, NUM_TOKENS, LATENT_CHANNELS, dtype=torch.float32)
    with torch.no_grad():
        kv_ref = vae_lat(lat1)
    log("contract", f"kv_cache shape={tuple(kv_ref.shape)} "
                    f"(expect [1,{NUM_TOKENS},{VAE_WIDTH_DECODER}])")
    vae_lat_path = os.path.join(args.out, "triposg_vae_latents.onnx")
    export_onnx(vae_lat, (lat1,), vae_lat_path,
                ["latents"], ["kv_cache"], {},
                args.opset)

    # ---- (d) VAE point query (chunked) ----------------------------------------
    vae_q = VaeQueryWrapper(pipe.vae).eval()
    P = 4096
    dummy_pts = (torch.rand(1, P, 3, dtype=torch.float32) * 2 - 1) * DECODE_BOUNDS
    with torch.no_grad():
        sdf_ref = vae_q(kv_ref, dummy_pts)
    log("contract", f"sdf shape={tuple(sdf_ref.shape)} (expect [1,{P},1]) "
                    f"range=({sdf_ref.min():.3f},{sdf_ref.max():.3f})")
    vae_q_path = os.path.join(args.out, "triposg_vae_decoder.onnx")
    export_onnx(vae_q, (kv_ref, dummy_pts), vae_q_path,
                ["kv_cache", "points"], ["sdf"],
                {"points": {1: "P"}, "sdf": {1: "P"}},
                args.opset)

    # split-vs-upstream correctness gate (torch-level, cheap, always on):
    with torch.no_grad():
        sdf_upstream = pipe.vae.decode(lat1, sampled_points=dummy_pts).sample
    split_ok = torch.allclose(sdf_ref, sdf_upstream, atol=1e-4)
    log("verify" if split_ok else "warn",
        f"kv-split vs vae.decode(): match={split_ok} "
        f"max|diff|={float((sdf_ref - sdf_upstream).abs().max()):.3e}")
    if not split_ok:
        log("warn", "kv-cache split does NOT reproduce upstream decode — "
                    "ship the --monolithic graph instead and fix the split")

    # ---- optional monolithic reference graph ----------------------------------
    if args.monolithic:
        mono = VaeDecodeMonolithic(pipe.vae).eval()
        mono_path = os.path.join(args.out, "triposg_vae_decode_mono.onnx")
        export_onnx(mono, (lat1, dummy_pts), mono_path,
                    ["latents", "points"], ["sdf"],
                    {"points": {1: "P"}, "sdf": {1: "P"}},
                    args.opset)

    # ---- ORT verification ------------------------------------------------------
    if args.verify:
        import onnxruntime as ort

        def sess(p):
            return ort.InferenceSession(p, providers=["CPUExecutionProvider"])

        if not args.skip_image_encoder:
            s = sess(enc_path)
            e = s.run(None, {"image": dummy_img.numpy()})[0]
            rel = np.abs(e - embeds_ref.numpy()).max() / (np.abs(embeds_ref.numpy()).max() + 1e-9)
            log("verify", f"ORT image_encoder {e.shape} max-rel-err={rel:.2e}")

        s = sess(dit_path)
        v = s.run(None, {"latents": dummy_lat.numpy(),
                         "timestep": dummy_t.numpy(),
                         "image_embeds": dummy_cond.numpy()})[0]
        rel = np.abs(v - v_ref.numpy()).max() / (np.abs(v_ref.numpy()).max() + 1e-9)
        log("verify", f"ORT dit_step {v.shape} max-rel-err={rel:.2e}")
        # dynamic-batch check (B=1 must also run — the two-call CFG variant)
        v1 = s.run(None, {"latents": dummy_lat[:1].numpy(),
                          "timestep": dummy_t[:1].numpy(),
                          "image_embeds": dummy_cond[:1].numpy()})[0]
        log("verify", f"ORT dit_step B=1 {v1.shape} "
                      f"match-B2-row0={np.allclose(v1, v[:1], atol=1e-3)}")

        s = sess(vae_lat_path)
        kv = s.run(None, {"latents": lat1.numpy()})[0]
        rel = np.abs(kv - kv_ref.numpy()).max() / (np.abs(kv_ref.numpy()).max() + 1e-9)
        log("verify", f"ORT vae_latents {kv.shape} max-rel-err={rel:.2e}")

        s = sess(vae_q_path)
        d = s.run(None, {"kv_cache": kv, "points": dummy_pts.numpy()})[0]
        rel = np.abs(d - sdf_ref.numpy()).max() / (np.abs(sdf_ref.numpy()).max() + 1e-9)
        log("verify", f"ORT vae_decoder {d.shape} max-rel-err={rel:.2e}")
        # dynamic-P check with an odd chunk size
        d2 = s.run(None, {"kv_cache": kv,
                          "points": dummy_pts[:, :777].numpy()})[0]
        log("verify", f"ORT vae_decoder P=777 {d2.shape} "
                      f"match={np.allclose(d2, d[:, :777], atol=1e-3)}")

        if not args.no_quant and os.path.exists(
                os.path.join(args.out, "triposg_dit_step_int8.onnx")):
            s = sess(os.path.join(args.out, "triposg_dit_step_int8.onnx"))
            vq = s.run(None, {"latents": dummy_lat.numpy(),
                              "timestep": dummy_t.numpy(),
                              "image_embeds": dummy_cond.numpy()})[0]
            rel = np.abs(vq - v_ref.numpy()).max() / (np.abs(v_ref.numpy()).max() + 1e-9)
            log("verify", f"ORT dit_step_int8 {vq.shape} max-rel-err={rel:.2e} "
                          f"(int8 — expect noticeably larger than fp32)")

        if args.monolithic:
            s = sess(os.path.join(args.out, "triposg_vae_decode_mono.onnx"))
            dm = s.run(None, {"latents": lat1.numpy(),
                              "points": dummy_pts.numpy()})[0]
            log("verify", f"ORT vae_decode_mono {dm.shape} "
                          f"match-split={np.allclose(dm, d, atol=1e-3)}")

    log("done", "export complete. Host the .onnx files (and the DiT "
                ".onnx.data sidecar) under triposg/ on the HF models repo.")


if __name__ == "__main__":
    main()
