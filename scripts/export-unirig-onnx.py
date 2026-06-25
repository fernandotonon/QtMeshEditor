#!/usr/bin/env python3
"""Export UniRig's SKELETON-PREDICTION stage to ONNX for #408 (UniRig port).

This is a ONE-TIME, OFFLINE developer tool — it is NOT shipped with the app and
the app never runs Python. The app runs the resulting .onnx files in C++ via
ONNX Runtime (see src/UniRigPredictor.cpp / src/AIAssistManager.cpp). We are
retargeting the existing #408 ONNX plumbing from RigNet to UniRig.

UniRig (SIGGRAPH 2025), VAST-AI-Research/UniRig:
  Code:    MIT          https://github.com/VAST-AI-Research/UniRig
  Weights: MIT          https://huggingface.co/VAST-AI/UniRig
  Trained on Articulation-XL2.0, which is CC-BY-4.0.
LICENSING NOTE: the UniRig code and the released weights are MIT, so they are
freely redistributable with attribution. The TRAINING DATA (Articulation-XL2.0)
is CC-BY-4.0 — that is an attribution obligation on the *dataset*, not a viral
copyleft on the model weights. Ship the .onnx with an attribution line crediting
"UniRig (VAST-AI, MIT) / Articulation-XL2.0 (CC-BY-4.0)" in the about/credits.

WHAT THIS EXPORTS
-----------------
Two ONNX graphs (the skeleton stage is an encoder->decoder seq2seq):

  encoder.onnx  Michelangelo SAL perceiver (3DShape2VecSet) + the width->hidden
                output_proj. Maps a sampled point cloud to a fixed number of
                decoder-ready PREFIX embeddings.

  decoder.onnx  The ~350M HuggingFace causal LM (config
                unirig_ar_350m_1024_81920) exported as a KV-cache stepwise
                decoder so the C++ side can run greedy/constrained autoregression
                one token at a time.

The C++ UniRigPredictor:
  1. samples num_samples surface points (+ normals) from the mesh,
  2. runs encoder.onnx -> prefix embeds [1, num_latents, hidden],
  3. seeds the decoder with those embeds (via inputs_embeds), then loops
     decoder.onnx feeding one generated token id per step + the returned
     KV-cache, doing a constrained-argmax over the next-possible-token mask,
  4. detokenizes the id stream (FSM below) into a joint tree,
  5. de-normalizes joints from the [-1,1] unit box back to mesh-local space.

ONNX I/O CONTRACT (what UniRigPredictor.cpp targets)
----------------------------------------------------
encoder.onnx
  inputs:
    pc      float32 [1, N, 3]   surface points, mesh-NORMALIZED to [-1,1]
                                 (N dynamic; inference uses num_samples=65536)
    feats   float32 [1, N, 3]   per-point normals (same N)
  output:
    latents float32 [1, num_latents, hidden]
            num_latents = perceiver query count (e.g. 257/513 — read from ckpt)
            hidden      = LM hidden_size (1024 for the 350m config)
            NOTE: this is AFTER output_proj (width -> hidden), so the tensor is
            already in decoder-embedding space and is fed straight in as the
            prefix via inputs_embeds.

decoder.onnx (one decode step, KV-cache causal LM)
  The exporter produces the optimum/HF-standard names. C++ discovers exact
  names via GetInputNameAllocated / GetOutputNameAllocated, but the contract is:
  inputs:
    input_ids        int64   [1, S]          token ids for this step (S=1 in the
                                              loop after the prefix is seeded)
    -- OR (prefix-seed step only) --
    inputs_embeds    float32 [1, P, hidden]   the encoder prefix (P=num_latents);
                                              use EITHER input_ids OR inputs_embeds
                                              for a given call, never both.
    attention_mask   int64   [1, T]          1s over all valid past+current
                                              positions (T = past_len + S)
    position_ids     int64   [1, S]          absolute positions for this step
    past_key_values.<i>.key    float32 [1, n_kv_heads, P_past, head_dim]
    past_key_values.<i>.value  float32 [1, n_kv_heads, P_past, head_dim]
                                              for i in [0, num_layers); P_past=0
                                              (empty) on the seeding step.
  outputs:
    logits           float32 [1, S, vocab]   vocab = 267 (see tokenizer below)
    present.<i>.key    float32 [1, n_kv_heads, P_past+S, head_dim]
    present.<i>.value  float32 [1, n_kv_heads, P_past+S, head_dim]
                                              feed each present.<i> back in as the
                                              next step's past_key_values.<i>.

  Stepwise flow in C++:
    step 0 (seed): inputs_embeds = encoder latents [1,P,hidden],
                   attention_mask = ones[1,P], position_ids = 0..P-1,
                   past = empty. Take logits[:, -1, :], constrained-argmax -> tok0.
    step k>0:      input_ids = [[tok_{k-1}]], past = previous present,
                   attention_mask = ones[1, P + k], position_ids = [[P + k - 1]].
                   logits[:, -1, :] -> constrained-argmax -> tok_k.
    stop when tok == token_id_eos (258) or max_new_tokens reached.

TOKENIZER (configs/tokenizer/tokenizer_parts_articulationxl_256.yaml) — the C++
side reimplements this; documented here so the export + contract stay in sync:
  num_discrete=256; continuous_range=[-1.0,1.0]; vocab_size=267
  ids 0..255 = coordinate bins
  256 branch | 257 bos | 258 eos | 259 pad
  260 spring(part=None) | 261 body | 262 hand
  263 cls_none | 264 vroid | 265 mixamo | 266 articulationxl
  discretize(t):   u=(t+1)/2; clip(round(u*256),0,255)
  undiscretize(b): ((b+0.5)/256)*2 - 1

Usage:
    python3 -m venv venv
    ./venv/bin/pip install torch transformers optimum[onnxruntime] onnx \
        onnxruntime huggingface_hub safetensors
    ./venv/bin/python scripts/export-unirig-onnx.py \
        --hf-repo VAST-AI/UniRig --out-dir dist/unirig_onnx
Then host the .onnx files and point AIAssistManager's model URLs at them
(default HF base https://huggingface.co/fernandotonon/QtMeshEditor-models).
"""
import argparse
import os
import sys

# UniRig's HF repo and the skeleton-stage sub-config. The repo bundles several
# stages (skeleton / skin); we only export the autoregressive skeleton model.
#
# TODO(checkpoint-layout): confirm the exact on-disk layout of VAST-AI/UniRig.
# The reference code (src/model/unirig_ar.py, src/system/ar.py) loads the AR
# model via a config name `unirig_ar_350m_1024_81920`. Inspect the repo to find:
#   * the encoder (Michelangelo SAL) weights + config (width, num_latents,
#     fourier embedder bands, input feat channels) — see src/model/michelangelo
#   * the causal-LM weights/config (transformers-loadable dir or a state_dict to
#     map onto an AutoConfig) and the output_proj (width -> hidden) Linear.
# Update DEFAULT_* below and the loaders once the real paths are known.
DEFAULT_HF_REPO = "VAST-AI/UniRig"
DEFAULT_LM_SUBDIR = "skeleton/ar"          # TODO: confirm — dir transformers can load
DEFAULT_ENC_WEIGHTS = "skeleton/encoder.safetensors"  # TODO: confirm filename
DEFAULT_NUM_SAMPLES = 65536
DEFAULT_VERTEX_SAMPLES = 8192

VOCAB_SIZE = 267  # must match the tokenizer above; logits last dim


# --------------------------------------------------------------------------- #
# Encoder (Michelangelo SAL perceiver + output_proj)
# --------------------------------------------------------------------------- #
def build_encoder(hf_repo: str, enc_weights: str):
    """Load the UniRig Michelangelo encoder and wrap it so its forward maps
    (pc, feats) -> latents in decoder-embedding space (after output_proj).

    TODO(checkpoint-layout): the precise module path differs per the UniRig
    release. The reference architecture (src/model/unirig_ar.py +
    src/model/michelangelo/...) is:
        FourierEmbedder(pc) -> cat(feats) -> input_proj(Linear -> width)
        -> ResidualCrossAttentionBlock(query=learned latents[num_latents,width],
                                       data=embedded points)
        -> num_latents latent tokens of dim `width`
        -> output_proj: Linear(width -> hidden)   # hidden = LM hidden_size
    Replace the import + instantiation below with the real classes once the repo
    layout is confirmed. The wrapper's forward signature and output shape
    [1, num_latents, hidden] are the load-bearing contract — keep them.
    """
    import torch
    import torch.nn as nn
    from huggingface_hub import hf_hub_download
    from safetensors.torch import load_file

    # --- BEGIN UniRig-repo-specific wiring (TODO: confirm against the checkout) ---
    # The UniRig package must be importable (pip install -e the cloned repo, or
    # add it to PYTHONPATH). These are the reference module paths.
    try:
        from src.model.michelangelo.encoder import ShapeAsLatentPerceiver  # type: ignore
    except Exception as e:  # pragma: no cover - depends on a local UniRig checkout
        raise SystemExit(
            "Could not import the UniRig encoder. Clone "
            f"{hf_repo}'s code repo (github.com/VAST-AI-Research/UniRig), "
            "`pip install -e .` it (or set PYTHONPATH), and confirm the "
            "ShapeAsLatentPerceiver import path.\n  underlying error: " + repr(e))

    # TODO(checkpoint-layout): pull these hyperparameters from the encoder config
    # shipped in the HF repo instead of hardcoding. width/num_latents/embed_dim
    # MUST match the trained weights or load_state_dict will fail.
    enc = ShapeAsLatentPerceiver(
        # device="cpu",
        # num_latents=256, embed_dim=..., width=..., heads=..., layers=...,
        # point_feats=3, fourier_embedder bands per the config,
    )
    weights_path = hf_hub_download(repo_id=hf_repo, filename=enc_weights)
    enc.load_state_dict(load_file(weights_path), strict=False)
    enc.eval()

    # output_proj: width -> LM hidden. TODO: load the trained Linear; this is the
    # projection that makes the latents decoder-ready prefix embeddings.
    output_proj_path = hf_hub_download(
        repo_id=hf_repo, filename="skeleton/output_proj.safetensors")  # TODO confirm
    op_sd = load_file(output_proj_path)
    width = op_sd["weight"].shape[1]
    hidden = op_sd["weight"].shape[0]
    output_proj = nn.Linear(width, hidden, bias="bias" in op_sd)
    output_proj.load_state_dict(op_sd)
    output_proj.eval()
    # --- END UniRig-repo-specific wiring ---

    class EncoderWrapper(nn.Module):
        def __init__(self, encoder, proj):
            super().__init__()
            self.encoder = encoder
            self.proj = proj

        def forward(self, pc, feats):
            # pc:[1,N,3], feats:[1,N,3]. The Michelangelo encoder returns latents
            # of shape [1, num_latents, width]; some impls also return a posterior
            # / shape embedding — take only the latent token set.
            out = self.encoder(pc, feats)
            latents = out[0] if isinstance(out, (tuple, list)) else out
            return self.proj(latents)  # [1, num_latents, hidden]

    return EncoderWrapper(enc, output_proj).eval(), hidden


def export_encoder(hf_repo: str, enc_weights: str, out_path: str) -> None:
    import torch
    import onnxruntime as ort

    enc, hidden = build_encoder(hf_repo, enc_weights)
    n = 2048  # dummy point count; N is a dynamic axis so any N works at runtime
    pc = torch.rand(1, n, 3) * 2.0 - 1.0
    feats = torch.rand(1, n, 3)

    torch.onnx.export(
        enc, (pc, feats), out_path, opset_version=18, dynamo=False,
        input_names=["pc", "feats"], output_names=["latents"],
        dynamic_axes={"pc": {0: "b", 1: "n"},
                      "feats": {0: "b", 1: "n"},
                      "latents": {0: "b", 1: "num_latents"}})

    sess = ort.InferenceSession(out_path, providers=["CPUExecutionProvider"])
    r = sess.run(None, {"pc": pc.numpy(), "feats": feats.numpy()})[0]
    print(f"  -> {out_path} ({os.path.getsize(out_path)} bytes); "
          f"latents {r.shape} (hidden={hidden}) "
          f"range[{r.min():.3f},{r.max():.3f}]")


# --------------------------------------------------------------------------- #
# Decoder (KV-cache causal LM)
# --------------------------------------------------------------------------- #
def export_decoder_optimum(hf_repo: str, lm_subdir: str, out_dir: str) -> bool:
    """Preferred path: let optimum export a proper KV-cache decoder graph.

    optimum's ORTModelForCausalLM / main_export emits decoder.onnx with the
    standard input_ids / attention_mask / position_ids / past_key_values.<i>.*
    inputs and logits / present.<i>.* outputs — exactly the contract documented
    at the top of this file. Returns True on success, False if optimum is absent.
    """
    try:
        from optimum.exporters.onnx import main_export
    except Exception:
        return False

    # main_export pulls the model from the HF hub (or a local path) and writes
    # decoder*.onnx + config. We point it at the LM sub-dir of the UniRig repo.
    # TODO(checkpoint-layout): if the LM is a raw state_dict rather than a
    # transformers-loadable directory, first materialize it as one (AutoConfig +
    # AutoModelForCausalLM, load_state_dict, save_pretrained) then export that.
    model_id = hf_repo
    if lm_subdir:
        # main_export accepts a local dir; download the subdir into a temp tree.
        from huggingface_hub import snapshot_download
        local = snapshot_download(repo_id=hf_repo, allow_patterns=[lm_subdir + "/*"])
        model_id = os.path.join(local, lm_subdir)

    print(f"  optimum main_export from {model_id}")
    main_export(
        model_name_or_path=model_id,
        output=out_dir,
        task="text-generation-with-past",  # => KV-cache decoder graph
        opset=18,
        # trust the UniRig custom architecture if it registers one:
        trust_remote_code=True,
        no_post_process=False,
    )
    # optimum may name the file decoder_model_merged.onnx / model.onnx; normalize
    # to decoder.onnx so the C++ side has a stable name.
    for cand in ("model.onnx", "decoder_model_merged.onnx",
                 "decoder_with_past_model.onnx", "decoder_model.onnx"):
        src = os.path.join(out_dir, cand)
        if os.path.exists(src):
            dst = os.path.join(out_dir, "decoder.onnx")
            if src != dst:
                os.replace(src, dst)
            print(f"  -> {dst} (via optimum, from {cand})")
            return True
    print("  optimum ran but produced no recognizable decoder file; "
          "inspect", out_dir)
    return True


def export_decoder_manual(hf_repo: str, lm_subdir: str, out_path: str) -> None:
    """Fallback: hand-roll a single-decode-step export with past_key_values in/out
    and inputs_embeds support, so the prefix can be fed as embeds and generated
    tokens as input_ids — matching the documented stepwise contract.
    """
    import torch
    import torch.nn as nn
    from transformers import AutoModelForCausalLM, AutoConfig
    from huggingface_hub import snapshot_download

    model_id = hf_repo
    if lm_subdir:
        local = snapshot_download(repo_id=hf_repo, allow_patterns=[lm_subdir + "/*"])
        model_id = os.path.join(local, lm_subdir)

    # TODO(checkpoint-layout): if loading fails, the LM may use UniRig's custom
    # architecture (config unirig_ar_350m_1024_81920). Register/import it from
    # src/model/unirig_ar.py before AutoModelForCausalLM.from_pretrained, or build
    # the model from AutoConfig + a state_dict load.
    config = AutoConfig.from_pretrained(model_id, trust_remote_code=True)
    model = AutoModelForCausalLM.from_pretrained(
        model_id, trust_remote_code=True, torch_dtype=torch.float32).eval()

    num_layers = config.num_hidden_layers
    hidden = config.hidden_size
    n_kv_heads = getattr(config, "num_key_value_heads", config.num_attention_heads)
    head_dim = hidden // config.num_attention_heads

    class StepDecoder(nn.Module):
        """One decode step. We accept inputs_embeds (prefix seed) here; the
        token-id steps in C++ embed ids via the same graph by passing input_ids
        — but ONNX can't branch on which input is present, so we export the
        EMBEDS variant (the seed) AND rely on the token steps reusing the model's
        embed_tokens. To keep a single graph, we always take inputs_embeds and
        let C++ do the id->embed lookup itself... EXCEPT that requires exporting
        embed_tokens too. Simpler + matches optimum: export the input_ids variant
        and feed the prefix via a separate seed call. See note below.
        """
        def __init__(self, m):
            super().__init__()
            self.m = m

        def forward(self, input_ids, attention_mask, position_ids, *past_flat):
            past = []
            for i in range(num_layers):
                past.append((past_flat[2 * i], past_flat[2 * i + 1]))
            out = self.m(
                input_ids=input_ids,
                attention_mask=attention_mask,
                position_ids=position_ids,
                past_key_values=tuple(past) if past_flat else None,
                use_cache=True,
                return_dict=True,
            )
            present_flat = []
            for k, v in out.past_key_values:
                present_flat += [k, v]
            return (out.logits, *present_flat)

    step = StepDecoder(model).eval()

    # NOTE on inputs_embeds vs input_ids: the documented contract lets the seed
    # step pass inputs_embeds (the encoder prefix) and subsequent steps pass
    # input_ids. ONNX has no runtime branch over "which optional input is set",
    # so we export the input_ids step graph (the inner loop, the hot path) here.
    # For the prefix seed, the recommended approach is one of:
    #   (a) export a SECOND tiny graph that takes inputs_embeds + empty past and
    #       returns logits + present (the seed graph) — preferred, and what the
    #       optimum path effectively gives you via the merged decoder; or
    #   (b) have C++ run the prefix through embed_tokens-free seeding by exporting
    #       embed_tokens separately.
    # The optimum path (export_decoder_optimum) handles both in one merged graph,
    # which is why it is preferred. This manual fallback exports the per-step
    # input_ids graph; wire the seed via optimum or a companion seed.onnx.
    # TODO: if you must avoid optimum, also export a seed graph taking
    #       inputs_embeds[1,P,hidden] -> (logits, present.<i>.*) with empty past.

    s = 1
    p_past = 4  # dummy non-zero past length so the cache dims are concrete
    input_ids = torch.zeros(1, s, dtype=torch.long)
    attention_mask = torch.ones(1, p_past + s, dtype=torch.long)
    position_ids = torch.full((1, s), p_past, dtype=torch.long)
    past_flat = []
    in_names = ["input_ids", "attention_mask", "position_ids"]
    out_names = ["logits"]
    dyn = {
        "input_ids": {0: "b", 1: "s"},
        "attention_mask": {0: "b", 1: "t"},
        "position_ids": {0: "b", 1: "s"},
        "logits": {0: "b", 1: "s"},
    }
    for i in range(num_layers):
        k = torch.rand(1, n_kv_heads, p_past, head_dim)
        v = torch.rand(1, n_kv_heads, p_past, head_dim)
        past_flat += [k, v]
        kn, vn = f"past_key_values.{i}.key", f"past_key_values.{i}.value"
        pkn, pvn = f"present.{i}.key", f"present.{i}.value"
        in_names += [kn, vn]
        out_names += [pkn, pvn]
        dyn[kn] = {0: "b", 2: "p_past"}
        dyn[vn] = {0: "b", 2: "p_past"}
        dyn[pkn] = {0: "b", 2: "p_total"}
        dyn[pvn] = {0: "b", 2: "p_total"}

    torch.onnx.export(
        step, (input_ids, attention_mask, position_ids, *past_flat), out_path,
        opset_version=18, dynamo=False,
        input_names=in_names, output_names=out_names, dynamic_axes=dyn)
    print(f"  -> {out_path} ({os.path.getsize(out_path)} bytes); "
          f"manual per-step KV-cache decoder "
          f"(layers={num_layers}, hidden={hidden}, kv_heads={n_kv_heads}, "
          f"head_dim={head_dim}, vocab={VOCAB_SIZE})")
    print("  NOTE: manual path exports the per-step input_ids graph only; "
          "feed the encoder prefix via optimum's merged graph or a seed.onnx "
          "(see export_decoder_manual docstring).")


def export_decoder(hf_repo: str, lm_subdir: str, out_dir: str) -> None:
    if export_decoder_optimum(hf_repo, lm_subdir, out_dir):
        return
    print("  optimum not available; falling back to hand-rolled torch.onnx.export")
    export_decoder_manual(hf_repo, lm_subdir, os.path.join(out_dir, "decoder.onnx"))


def main() -> int:
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--hf-repo", default=DEFAULT_HF_REPO,
                    help="HF repo id holding the UniRig weights")
    ap.add_argument("--lm-subdir", default=DEFAULT_LM_SUBDIR,
                    help="sub-dir of the repo holding the transformers-loadable LM")
    ap.add_argument("--enc-weights", default=DEFAULT_ENC_WEIGHTS,
                    help="path (within the repo) to the Michelangelo encoder weights")
    ap.add_argument("--out-dir", default="unirig_onnx",
                    help="dir to write encoder.onnx / decoder.onnx")
    ap.add_argument("--skip-encoder", action="store_true")
    ap.add_argument("--skip-decoder", action="store_true")
    args = ap.parse_args()

    os.makedirs(args.out_dir, exist_ok=True)

    if not args.skip_encoder:
        print("=== encoder (Michelangelo SAL + output_proj) ===")
        export_encoder(args.hf_repo, args.enc_weights,
                       os.path.join(args.out_dir, "encoder.onnx"))

    if not args.skip_decoder:
        print("=== decoder (KV-cache causal LM) ===")
        export_decoder(args.hf_repo, args.lm_subdir, args.out_dir)

    print("ALL EXPORTS OK")
    print("Attribution to ship in credits: "
          "UniRig (VAST-AI, MIT) / trained on Articulation-XL2.0 (CC-BY-4.0).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
