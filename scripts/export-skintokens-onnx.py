#!/usr/bin/env python3
"""Export SkinTokens/TokenRig's skinning path to ONNX for #819 Slice C.

ONE-TIME, OFFLINE developer tool — NOT shipped with the app, and the app never
runs Python. The app runs the resulting .onnx files in C++ via ONNX Runtime
(src/SkinTokensPredictor.cpp). This script produces the hosted models; re-run
it to regenerate or update them.

MODEL: SkinTokens / TokenRig (VAST-AI-Research/SkinTokens) — "a learned,
  compact, discrete representation for skinning weights" + a unified
  autoregressive rig transformer. Code **MIT**; weights **MIT**
  (HF VAST-AI/SkinTokens, license tag verified 2026-07); the AR backbone is
  Qwen3-0.6B (Apache-2.0). Attribution: THIRD_PARTY_AI_MODELS.md.

WHY SKINTOKENS (not UniRig's own skin head): UniRig's skin stage runs PTv3 on
  spconv sparse convolutions — no ONNX lowering exists. SkinTokens has NO
  spconv (verified: fps is pure torch in-repo, flash-attn has SDPA fallbacks,
  Michelangelo has a `flash: False` config path), so it exports with the same
  recipe as the #408 UniRig skeleton export.

WHAT IT PRODUCES (the exact I/O contract src/SkinTokensPredictor.cpp targets):
  mesh_cond.onnx  : vertices[1,N,3], normals[1,N,3]      -> cond_embeds[1,C,H]
                    (Michelangelo encode_latents + output_proj — the LLM's
                     mesh conditioning prefix; C = tokens_skin_cond? no — C is
                     the encoder's latent token count, H = llm hidden size)
  vae_cond.onnx   : cond[1,N,6] (xyz|normal)             -> cond_latents[1,K,D]
                    (SkinFSQCVAE cond encoder; K = tokens_skin_cond,
                     D = vae latent_channels)
  embed.onnx      : input_ids[1,S]                       -> embeds[1,S,H]
  decoder.onnx    : inputs_embeds[1,S,H],
                    past.{i}.{key,value}[1,KV,P,Dh]      -> logits[1,S,V],
                                                            present.{i}.{k,v}
                    (Qwen3-0.6B causal step, eager attention, fp32)
  skin_decode.onnx: skin_ids[1,tokens_per_skin] (FSQ ids, 0-based),
                    cond[1,N,6], cond_latents[1,K,D]     -> weights[1,N]
                    (FSQ indices_to_codes folded in + optional up_perceiver +
                     SkinFSQCVAE._decode; one call per JOINT)
  skintokens.json : every config value the C++ runtime needs (vocab layout,
                    tokens_per_skin, tokens_skin_cond, num_discrete,
                    continuous_range, cls token map, parts token map,
                    sample counts, llm dims) + the transform normalization.

SEQUENCE CONTRACT (TokenRig.generate, teacher-forced skeleton):
  inputs_embeds = [ mesh_cond embeds | embed(bos, cls, skeleton tokens…,
                    tokenizer.eos) ]
  then AR-decode J*tokens_per_skin skin tokens (ids in
  [tokenizer.vocab_size, tokenizer.vocab_size + fsq_codebook)), then the
  global EOS (= tokenizer.vocab_size + fsq_codebook). Per joint j, its
  tokens_per_skin ids (minus tokenizer.vocab_size) feed skin_decode.onnx to
  get the per-sampled-point weight column; per-vertex weights = 8-NN
  inverse-distance transfer from sampled points (Asset.from_data recipe).

DTYPE: the checkpoint is bf16; everything is cast to fp32 for export (ONNX
  bf16 support is poor and CPU EPs want fp32 anyway).

RANDOMNESS: the VAE cond encoder subsamples its query points with a host-side
  numpy RNG + pure-torch fps. Under tracing those indices freeze into the
  graph — acceptable because the C++ caller already feeds an
  independently-sampled, fixed-size point set, so a frozen subsample of it is
  statistically the same thing. Feed EXACTLY --num-points points at runtime.

ENVIRONMENT (what was actually needed; CPU-only is fine):
  cd SkinTokens
  python3 -m venv venv
  ./venv/bin/pip install torch --index-url https://download.pytorch.org/whl/cpu
  ./venv/bin/pip install "transformers>=4.57" "diffusers>=0.35" python-box \
      einops omegaconf lightning addict trimesh huggingface_hub numpy onnx \
      onnxruntime scipy
  ./venv/bin/python -c "from huggingface_hub import hf_hub_download as d; \
      d('VAST-AI/SkinTokens','experiments/skin_vae_2_10_32768/last.ckpt',local_dir='.'); \
      d('VAST-AI/SkinTokens','experiments/articulation_xl_quantization_256_token_4/grpo_1400.ckpt',local_dir='.')"
  ./venv/bin/python -c "from huggingface_hub import snapshot_download as s; \
      s('Qwen/Qwen3-0.6B', local_dir='models/Qwen3-0.6B', \
        allow_patterns=['config.json','generation_config.json','tokenizer*'])"
  ./venv/bin/python <this script> --repo . \
      --ckpt experiments/articulation_xl_quantization_256_token_4/grpo_1400.ckpt \
      --out-dir dist/skintokens_onnx

Then upload the .onnx files + skintokens.json to the HF models repo under
skintokens/ (the default base URL in src/SkinTokensPredictor.cpp).
"""
import argparse
import json
import os
import sys

import numpy as np
import torch
import torch.nn as nn


def log(msg):
    print(f"[export-skintokens] {msg}", flush=True)


def install_flash_attn_stub():
    """skin_vae_model.py imports flash_attn with NO SDPA fallback (unlike
    attention_processor.py, which has one). Inject a pure-torch stub module
    BEFORE importing TokenRig. Contract (flash-attn native): q/k/v are
    (B, L, H, D); returns (out, lse). The Perceiver force-casts q to bf16
    even in an fp32 model, so the stub computes in fp32 and returns in the
    VALUE dtype (the projections' dtype) to avoid a bf16×fp32 matmul error.
    """
    import types
    mod = types.ModuleType("flash_attn_interface")

    def flash_attn_func(q, k, v, *args, **kwargs):
        qt = q.permute(0, 2, 1, 3).float()
        kt = k.permute(0, 2, 1, 3).float()
        vt = v.permute(0, 2, 1, 3).float()
        if qt.shape[1] != kt.shape[1]:
            rep = qt.shape[1] // kt.shape[1]
            kt = kt.repeat_interleave(rep, dim=1)
            vt = vt.repeat_interleave(rep, dim=1)
        out = torch.nn.functional.scaled_dot_product_attention(qt, kt, vt)
        return out.permute(0, 2, 1, 3).to(v.dtype), None

    mod.flash_attn_func = flash_attn_func
    # transformers probes importlib.util.find_spec("flash_attn_interface");
    # a bare injected module has __spec__=None which makes find_spec RAISE.
    # A real (loader-less) spec keeps the probe happy, and the missing dist
    # metadata still makes transformers report flash-attn as unavailable.
    import importlib.machinery
    mod.__spec__ = importlib.machinery.ModuleSpec(
        "flash_attn_interface", loader=None)
    sys.modules["flash_attn_interface"] = mod

    # TokenRig.__init__ HARDCODES attn_implementation="flash_attention_2"
    # on AutoModelForCausalLM.from_config — transformers raises when
    # flash-attn is absent. Wrap from_config to force eager (traceable).
    import transformers

    _orig_from_config = transformers.AutoModelForCausalLM.from_config.__func__

    def _eager_from_config(cls, config, **kw):
        kw["attn_implementation"] = "eager"
        return _orig_from_config(cls, config, **kw)

    transformers.AutoModelForCausalLM.from_config = classmethod(
        _eager_from_config)

    # CPU-only torch: michelangelo's FLASH3 helper probes
    # torch.cuda.get_device_name(0) at IMPORT time (crashes without
    # CUDA), and flash_attention() enters torch.backends.cuda.sdp_kernel
    # — stub both so the CPU export can trace the eager/SDPA paths.
    if not torch.cuda.is_available():
        import contextlib
        torch.cuda.get_device_name = lambda *a, **k: "CPU"
        torch.backends.cuda.sdp_kernel = (
            lambda **kw: contextlib.nullcontext())


def traced_fps(x, batch, ratio, random_start=False):
    """Trace-friendly farthest-point sampling for the export.

    The repo's fps() masks by batch id (dynamic shapes under tracing)
    and calls .item(). Upstream always calls it with a single uniform
    batch, so this drop-in works on the whole tensor with STATIC
    shapes: num_samples becomes a python int and the FPS loop unrolls
    into the graph (num_samples iterations of argmax over N).
    """
    del batch, random_start
    num_points = int(x.shape[0])
    num_samples = max(1, min(int(round(num_points * ratio)), num_points))
    distances = torch.full((num_points,), float("inf"), dtype=x.dtype)
    selected = []
    farthest = torch.zeros((), dtype=torch.long)
    for _ in range(num_samples):
        selected.append(farthest)
        centroid = x[farthest]
        dist = torch.sum((x - centroid) ** 2, dim=-1)
        distances = torch.minimum(distances, dist)
        farthest = torch.argmax(distances)
    return torch.stack(selected)


def build_model(repo: str, ckpt: str):
    install_flash_attn_stub()
    sys.path.insert(0, os.path.abspath(repo))
    os.chdir(repo)
    from src.model.tokenrig import TokenRig

    # fps was imported BY VALUE into these modules — patch each site.
    import src.model.utils as _utils
    import src.model.skin_vae.autoencoders.skin_fsq_cvae_model as _cvae
    import src.model.michelangelo.models.tsal.sal_perceiver as _salp
    _utils.fps = traced_fps
    _cvae.fps = traced_fps
    _salp.fps = traced_fps

    log(f"loading checkpoint {ckpt} (bf16 → fp32)…")
    model = TokenRig.load_from_system_checkpoint(checkpoint_path=ckpt)
    model.eval()
    # Whole pipeline to fp32 on CPU: ONNX + CPU EP want fp32.
    model = model.float().cpu()
    model.vae = model.vae.float().cpu()
    model.mesh_encoder = model.mesh_encoder.float().cpu()
    model.output_proj = model.output_proj.float().cpu()

    # Swap the LLM to eager attention for a traceable graph.
    try:
        model.transformer.config._attn_implementation = "eager"
        for m in model.transformer.modules():
            if hasattr(m, "config") and hasattr(m.config, "_attn_implementation"):
                m.config._attn_implementation = "eager"
    except Exception as e:
        log(f"warning: could not force eager attention: {e}")
    return model


class DecomposedRMSNorm(nn.Module):
    """nn.RMSNorm lowers to aten::rms_norm, which the torchscript
    exporter can't map at opset 18 — decompose it into primitive ops
    (numerically identical)."""

    def __init__(self, src: nn.RMSNorm):
        super().__init__()
        self.weight = src.weight
        self.eps = src.eps if src.eps is not None else 1e-6

    def forward(self, x):
        var = x.pow(2).mean(-1, keepdim=True)
        x = x * torch.rsqrt(var + self.eps)
        return x * self.weight


def decompose_rmsnorm(module: nn.Module):
    for name, child in list(module.named_children()):
        if isinstance(child, nn.RMSNorm):
            setattr(module, name, DecomposedRMSNorm(child))
        else:
            decompose_rmsnorm(child)


class MeshCondWrapper(nn.Module):
    """vertices+normals -> LLM conditioning prefix (encode_mesh_cond)."""

    def __init__(self, mesh_encoder, output_proj):
        super().__init__()
        self.mesh_encoder = mesh_encoder
        self.output_proj = output_proj

    def forward(self, vertices, normals):
        shape_embed, latents, token_num, pre_pc = \
            self.mesh_encoder.encode_latents(pc=vertices, feats=normals)
        return self.output_proj(latents)


class VaeCondWrapper(nn.Module):
    """cond [1,N,6] -> cond_latents [1,K,D] (SkinFSQCVAE cond path)."""

    def __init__(self, vae_model, cond_tokens):
        super().__init__()
        self.vae_model = vae_model
        self.cond_tokens = cond_tokens

    def forward(self, cond):
        _, cond_latents = self.vae_model._encode(
            x=None, cond=cond, num_tokens=0,
            cond_tokens=self.cond_tokens, seed=0,
            return_z=False, return_cond=True)
        return cond_latents


class SkinDecodeWrapper(nn.Module):
    """FSQ skin ids (0-based) + cond + cond_latents -> per-point weights.

    Folds FSQ.indices_to_codes and the optional up_perceiver into the
    graph so the C++ side never touches FSQ math.
    """

    def __init__(self, vae, tokens_per_skin):
        super().__init__()
        self.vae = vae
        self.tokens_per_skin = tokens_per_skin

    def forward(self, skin_ids, cond, cond_latents):
        z = self.vae.model.FSQ.indices_to_codes(skin_ids)
        z = z.reshape(1, self.tokens_per_skin, -1)
        logits = self.vae.decode(z=z, sampled_cond=cond,
                                 cond_tokens=cond_latents)
        return logits.reshape(1, -1)


class EmbedWrapper(nn.Module):
    def __init__(self, transformer):
        super().__init__()
        self.embed = transformer.get_input_embeddings()

    def forward(self, input_ids):
        return self.embed(input_ids)


class DecoderStepWrapper(nn.Module):
    """Qwen3 causal step with explicit KV cache (the #408 driver shape)."""

    def __init__(self, transformer, num_layers):
        super().__init__()
        self.transformer = transformer
        self.num_layers = num_layers

    def forward(self, inputs_embeds, *past):
        from transformers.cache_utils import DynamicCache
        cache = DynamicCache()
        for i in range(self.num_layers):
            k = past[2 * i]
            v = past[2 * i + 1]
            cache.update(k, v, i)
        out = self.transformer(
            inputs_embeds=inputs_embeds,
            past_key_values=cache,
            use_cache=True,
            return_dict=True,
        )
        # transformers 5.x: DynamicCache holds per-layer objects with
        # .keys/.values (the 4.x key_cache/value_cache lists are gone).
        presents = []
        pkv = out.past_key_values
        for i in range(self.num_layers):
            if hasattr(pkv, "layers"):
                presents.append(pkv.layers[i].keys)
                presents.append(pkv.layers[i].values)
            else:   # transformers 4.x fallback
                presents.append(pkv.key_cache[i])
                presents.append(pkv.value_cache[i])
        return (out.logits, *presents)


def export_onnx(module, args, in_names, out_names, dynamic_axes, path, opset=18):
    log(f"exporting {os.path.basename(path)}…")
    with torch.no_grad():
        torch.onnx.export(
            module, args, path,
            input_names=in_names, output_names=out_names,
            dynamic_axes=dynamic_axes, opset_version=opset,
            do_constant_folding=True, dynamo=False,
        )
    import onnx
    onnx.checker.check_model(onnx.load(path, load_external_data=False))
    log(f"  ok: {path} ({os.path.getsize(path) / 1e6:.1f} MB)")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--repo", required=True, help="SkinTokens repo checkout")
    ap.add_argument("--ckpt", required=True, help="TokenRig .ckpt path (relative to repo)")
    ap.add_argument("--out-dir", default="dist/skintokens_onnx")
    ap.add_argument("--num-points", type=int, default=8192,
                    help="fixed sampled-point count baked into the traced graphs")
    ap.add_argument("--skip", default="", help="comma list of graphs to skip")
    args = ap.parse_args()

    out_dir = os.path.abspath(args.out_dir)
    os.makedirs(out_dir, exist_ok=True)
    skip = set(x for x in args.skip.split(",") if x)

    model = build_model(args.repo, args.ckpt)
    tok = model.tokenizer
    N = args.num_points

    llm_cfg = model.llm_config
    num_layers = llm_cfg.num_hidden_layers
    num_kv = llm_cfg.num_key_value_heads
    head_dim = getattr(llm_cfg, "head_dim",
                       llm_cfg.hidden_size // llm_cfg.num_attention_heads)

    # ── Config manifest for the C++ runtime ─────────────────────────
    manifest = {
        "schema": "qtmesh-skintokens-onnx-v1",
        "num_points": N,
        "tokens_per_skin": model.tokens_per_skin,
        "tokens_skin_cond": model.tokens_skin_cond,
        "vae_latent_channels": model.vae.latent_channels,
        "fsq_codebook_size": model.vae.vocab_size,
        "tokenizer": {
            "num_discrete": tok.num_discrete,
            "continuous_range": list(tok.continuous_range),
            "token_id_branch": tok.token_id_branch,
            "token_id_bos": tok.token_id_bos,
            "token_id_eos": tok.token_id_eos,
            "token_id_pad": tok.token_id_pad,
            "token_id_spring": tok.token_id_spring,
            "token_id_cls_none": tok.token_id_cls_none,
            "cls_token_id": dict(tok.cls_token_id),
            "parts_token_id": dict(tok.parts_token_id),
            "vocab_size": tok.vocab_size,
        },
        "llm": {
            "hidden_size": llm_cfg.hidden_size,
            "num_hidden_layers": num_layers,
            "num_key_value_heads": num_kv,
            "head_dim": head_dim,
            "full_vocab_size": model.vocab_size,
            "global_eos": model.eos,
        },
        "transform_config": model.transform_config.get("predict_transform")
            if isinstance(model.transform_config, dict) else None,
        "license": "MIT (code+weights, VAST-AI/SkinTokens); Qwen3-0.6B Apache-2.0",
    }
    manifest_path = os.path.join(out_dir, "skintokens.json")
    with open(manifest_path, "w") as f:
        json.dump(manifest, f, indent=2, default=str)
    log(f"wrote {manifest_path}")

    torch.manual_seed(0)
    np.random.seed(0)

    # ── mesh_cond.onnx ──────────────────────────────────────────────
    if "mesh_cond" not in skip:
        m = MeshCondWrapper(model.mesh_encoder, model.output_proj).eval()
        decompose_rmsnorm(m)
        v = torch.randn(1, N, 3)
        n = torch.nn.functional.normalize(torch.randn(1, N, 3), dim=-1)
        export_onnx(m, (v, n), ["vertices", "normals"], ["cond_embeds"],
                    {}, os.path.join(out_dir, "mesh_cond.onnx"))

    # ── vae_cond.onnx ───────────────────────────────────────────────
    if "vae_cond" not in skip:
        m = VaeCondWrapper(model.vae.model, model.tokens_skin_cond).eval()
        cond = torch.randn(1, N, 6)
        export_onnx(m, (cond,), ["cond"], ["cond_latents"],
                    {}, os.path.join(out_dir, "vae_cond.onnx"))

    # ── skin_decode.onnx ────────────────────────────────────────────
    if "skin_decode" not in skip:
        m = SkinDecodeWrapper(model.vae, model.tokens_per_skin).eval()
        ids = torch.zeros(1, model.tokens_per_skin, dtype=torch.long)
        cond = torch.randn(1, N, 6)
        lat = torch.randn(1, model.tokens_skin_cond,
                          model.vae.latent_channels)
        export_onnx(m, (ids, cond, lat),
                    ["skin_ids", "cond", "cond_latents"], ["weights"],
                    {}, os.path.join(out_dir, "skin_decode.onnx"))

    # ── embed.onnx ──────────────────────────────────────────────────
    if "embed" not in skip:
        m = EmbedWrapper(model.transformer).eval()
        ids = torch.zeros(1, 8, dtype=torch.long)
        export_onnx(m, (ids,), ["input_ids"], ["embeds"],
                    {"input_ids": {1: "seq"}, "embeds": {1: "seq"}},
                    os.path.join(out_dir, "embed.onnx"))

    # ── decoder.onnx ────────────────────────────────────────────────
    if "decoder" not in skip:
        m = DecoderStepWrapper(model.transformer, num_layers).eval()
        embeds = torch.randn(1, 4, llm_cfg.hidden_size)
        past = []
        in_names = ["inputs_embeds"]
        out_names = ["logits"]
        dyn = {"inputs_embeds": {1: "seq"}, "logits": {1: "seq"}}
        for i in range(num_layers):
            past.append(torch.randn(1, num_kv, 3, head_dim))
            past.append(torch.randn(1, num_kv, 3, head_dim))
            for kv in ("key", "value"):
                in_names.append(f"past.{i}.{kv}")
                out_names.append(f"present.{i}.{kv}")
                dyn[f"past.{i}.{kv}"] = {2: "past_seq"}
                dyn[f"present.{i}.{kv}"] = {2: "total_seq"}
        export_onnx(m, (embeds, *past), in_names, out_names, dyn,
                    os.path.join(out_dir, "decoder.onnx"))

    log("all requested graphs exported.")
    log("VALIDATE with --validate-run before hosting (see repo docs).")


if __name__ == "__main__":
    main()
