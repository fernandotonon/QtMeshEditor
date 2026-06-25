#!/usr/bin/env python3
"""Export UniRig's skeleton-prediction stage to ONNX for #408.

ONE-TIME, OFFLINE developer tool — NOT shipped with the app, and the app never
runs Python. The app runs the resulting .onnx files in C++ via ONNX Runtime
(src/UniRigPredictor.cpp). This script was used to produce the hosted models;
re-run it to regenerate or update them.

MODEL: UniRig (Zhang et al., SIGGRAPH 2025, "One Model to Rig Them All",
  VAST-AI-Research/UniRig). Code MIT; weights MIT (HF VAST-AI/UniRig); trained on
  Articulation-XL2.0 (CC-BY-4.0). Attribution belongs in app credits — see
  THIRD_PARTY_AI_MODELS.md.

WHAT IT PRODUCES (the exact I/O contract src/UniRigPredictor.cpp targets;
verified against the checkpoint — L=24 OPT-350m layers, vocab 267):
  encoder.onnx : vertices[1,N,3], normals[1,N,3]            -> latents[1,1024,1024]
  embed.onnx   : input_ids[1,S]                             -> token_embeds[1,S,1024]
  decoder.onnx : inputs_embeds[1,S,1024],
                 past.{0..23}.{key,value}[1,16,P,64]        -> logits[1,S,267],
                 present.{0..23}.{key,value}
  Conditioning (UniRigAR.generate): inputs_embeds = cat([latents,
  embed([bos, cls=articulation-xl(266)])]); then constrained greedy AR decode
  with the tokenizer FSM mask, detokenize to a skeleton tree.

WHY A CUSTOM DRIVER (not just optimum): the released checkpoint is a single
Lightning .ckpt (skeleton/articulation-xl_quantization_256/model.ckpt) holding
BOTH the Michelangelo encoder and the OPT-350m AR transformer; and UniRig
conditions the decoder with the encoder latents as *inputs_embeds* (not token
ids). So we build UniRigAR from the repo configs, load the ckpt, and export the
encoder + a custom inputs_embeds/KV-cache decoder step + the token-embed lookup.

ENVIRONMENT SETUP (what was actually needed; CPU is fine — slow but works):
  python3 -m venv venv
  ./venv/bin/pip install torch --index-url https://download.pytorch.org/whl/cpu
  ./venv/bin/pip install transformers "optimum[onnxruntime]" onnx onnxruntime \
      huggingface_hub safetensors numpy python-box pyyaml einops tqdm \
      fast_simplification scipy trimesh lightning addict timm
  git clone --depth 1 https://github.com/VAST-AI-Research/UniRig.git
  # Download the skeleton checkpoint (≈1.44 GB):
  ./venv/bin/python -c "from huggingface_hub import hf_hub_download as d; \
      d('VAST-AI/UniRig','skeleton/articulation-xl_quantization_256/model.ckpt', \
        local_dir='ckpt')"
  # UniRig's model package eagerly imports PTv3 + skin deps we never run
  # (spconv, torch_scatter, torch_cluster, flash_attn). Only the Michelangelo
  # encoder's farthest-point-sampling (torch_cluster.fps) is actually called, so
  # stub the rest and provide a pure-torch fps. See the repo issue / this script's
  # git history for the exact stub bodies (spconv.pytorch, torch_scatter,
  # flash_attn.modules.mha, and a real fps in torch_cluster).
  ./venv/bin/python scripts/export-unirig-onnx.py \
      --repo ./UniRig --ckpt ./ckpt/skeleton/articulation-xl_quantization_256/model.ckpt \
      --out-dir dist/unirig_onnx
Then upload encoder.onnx / decoder.onnx / embed.onnx to the HF models repo under
unirig/ (the default base URL in src/UniRigPredictor.cpp).
"""
import argparse
import glob
import os
import sys

import torch
import torch.nn as nn
import yaml
from box import Box


def build_model(repo: str, ckpt: str):
    sys.path.insert(0, repo)
    os.chdir(repo)   # configs reference skeleton_path relatively
    from src.model.parse import get_model
    from src.tokenizer.parse import get_tokenizer
    from src.tokenizer.spec import TokenizerConfig

    model_cfg = Box(yaml.safe_load(open("configs/model/unirig_ar_350m_1024_81920_float32.yaml")))
    tok_cfg   = Box(yaml.safe_load(open("configs/tokenizer/tokenizer_parts_articulationxl_256.yaml")))
    tokenizer = get_tokenizer(TokenizerConfig.parse(tok_cfg))

    md = model_cfg
    md.llm._attn_implementation = "eager"          # CPU export — no flash-attn
    if "flash" in md.mesh_encoder:
        md.mesh_encoder.flash = False
    model = get_model(tokenizer=tokenizer, **md)

    sd = torch.load(ckpt, map_location="cpu", weights_only=False)
    state = sd.get("state_dict", sd)
    cleaned = {(k[len("model."):] if k.startswith("model.") else k): v
               for k, v in state.items()}            # strip the LightningModule prefix
    missing, unexpected = model.load_state_dict(cleaned, strict=False)
    print(f"  ckpt loaded: {len(cleaned)} tensors; missing={len(missing)} unexpected={len(unexpected)}")
    model.eval()
    return model, tokenizer


def export_encoder(model, out_dir):
    print("=== encoder (Michelangelo SAL + output_proj) ===")
    class Enc(nn.Module):
        def __init__(s, m): super().__init__(); s.m = m
        def forward(s, vertices, normals):
            return s.m.encode_mesh_cond(vertices=vertices, normals=normals)
    enc = Enc(model).eval()
    v = torch.randn(1, 2048, 3); n = torch.randn(1, 2048, 3)
    with torch.no_grad():
        print("  encoder out:", tuple(enc(v, n).shape))
    torch.onnx.export(enc, (v, n), os.path.join(out_dir, "encoder.onnx"),
                      input_names=["vertices", "normals"], output_names=["latents"],
                      dynamic_axes={"vertices": {1: "N"}, "normals": {1: "N"}},
                      opset_version=17, dynamo=False)
    print("  wrote encoder.onnx")


def export_decoder(model, out_dir):
    # UniRig feeds the encoder latents as inputs_embeds — so export a custom
    # single-step graph taking inputs_embeds + a flat KV-cache (DynamicCache),
    # plus a token-embedding lookup the C++ uses to embed generated tokens.
    print("=== decoder (OPT-350m, inputs_embeds + KV-cache) ===")
    from transformers.cache_utils import DynamicCache
    lm = model.transformer.eval(); cfg = lm.config
    L, H = cfg.num_hidden_layers, cfg.num_attention_heads
    Dh = cfg.hidden_size // cfg.num_attention_heads

    class Step(nn.Module):
        def __init__(s, lm): super().__init__(); s.lm = lm
        def forward(s, inputs_embeds, *past):
            pkv = DynamicCache.from_legacy_cache(
                tuple((past[2*i], past[2*i+1]) for i in range(L))) if past else None
            out = s.lm(inputs_embeds=inputs_embeds, past_key_values=pkv,
                       use_cache=True, return_dict=True)
            flat = []
            for (k, v) in out.past_key_values.to_legacy_cache(): flat += [k, v]
            return (out.logits, *flat)

    emb = torch.randn(1, 4, cfg.hidden_size)
    past0 = sum(([torch.randn(1, H, 2, Dh), torch.randn(1, H, 2, Dh)] for _ in range(L)), [])
    in_names  = ["inputs_embeds"] + sum(([f"past.{i}.key", f"past.{i}.value"] for i in range(L)), [])
    out_names = ["logits"]        + sum(([f"present.{i}.key", f"present.{i}.value"] for i in range(L)), [])
    dyn = {"inputs_embeds": {1: "S"}, "logits": {1: "S"}}
    for i in range(L):
        dyn[f"past.{i}.key"] = {2: "P"}; dyn[f"past.{i}.value"] = {2: "P"}
        dyn[f"present.{i}.key"] = {2: "T"}; dyn[f"present.{i}.value"] = {2: "T"}
    with torch.no_grad():
        torch.onnx.export(Step(lm).eval(), (emb, *past0),
                          os.path.join(out_dir, "decoder.onnx"),
                          input_names=in_names, output_names=out_names,
                          dynamic_axes=dyn, opset_version=17, dynamo=False)
    print("  wrote decoder.onnx")

    class Embed(nn.Module):
        def __init__(s, lm): super().__init__(); s.e = lm.get_input_embeddings()
        def forward(s, input_ids): return s.e(input_ids)
    with torch.no_grad():
        torch.onnx.export(Embed(lm).eval(), (torch.zeros(1, 1, dtype=torch.long),),
                          os.path.join(out_dir, "embed.onnx"),
                          input_names=["input_ids"], output_names=["token_embeds"],
                          dynamic_axes={"input_ids": {1: "S"}, "token_embeds": {1: "S"}},
                          opset_version=17, dynamo=False)
    print("  wrote embed.onnx")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--repo", required=True, help="path to a UniRig checkout")
    ap.add_argument("--ckpt", required=True,
                    help="skeleton/articulation-xl_quantization_256/model.ckpt")
    ap.add_argument("--out-dir", default="unirig_onnx")
    ap.add_argument("--skip-encoder", action="store_true")
    ap.add_argument("--skip-decoder", action="store_true")
    args = ap.parse_args()
    out_dir = os.path.abspath(args.out_dir)
    os.makedirs(out_dir, exist_ok=True)
    try:
        import torch  # noqa: F401
    except ImportError:
        print("error: install torch + transformers + optimum first "
              "(see the module docstring).", file=sys.stderr)
        return 2
    model, tok = build_model(os.path.abspath(args.repo), os.path.abspath(args.ckpt))
    print("vocab_size:", tok.vocab_size)
    if not args.skip_encoder: export_encoder(model, out_dir)
    if not args.skip_decoder: export_decoder(model, out_dir)
    print("ALL EXPORTS OK ->", out_dir)
    print("Credits: UniRig (VAST-AI, MIT) / Articulation-XL2.0 (CC-BY-4.0).")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
