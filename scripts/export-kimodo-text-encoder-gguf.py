#!/usr/bin/env python3
"""Build Kimodo's text encoder as a GGUF for QtMeshEditor's built-in llama.cpp (dev tool, not shipped).

Kimodo conditions on LLM2Vec embeddings of Meta-Llama-3-8B-Instruct: the Llama weights with the MNTP and the
supervised LoRA folded in, run with BIDIRECTIONAL attention and mean-pooled over the prompt's tokens. This
script merges both LoRAs (LLM2Vec's own loader, so the merge is the one Kimodo runs), saves the result as a
headless Llama tagged `LlamaBidirectionalModel` (llama.cpp's `llama-embed` architecture: no output head,
non-causal), and converts it with llama.cpp's convert_hf_to_gguf.py.

The output is a derivative of Meta Llama 3: it is distributed under the Meta Llama 3 Community License, its
name must start with "Llama-3" and the licence + NOTICE travel with it (scripts/upload-kimodo-models.sh).

    python3 scripts/export-kimodo-text-encoder-gguf.py --llama-cpp build_local/_deps/llama_cpp-src \\
        --out ~/kimodo_export/Llama-3-LLM2Vec-8B-Kimodo-TextEncoder-Q8_0.gguf

Needs the user's own Hugging Face access to meta-llama/Meta-Llama-3-8B-Instruct (gated) and ~16 GB of free
disk for the intermediate bf16 checkpoint (deleted afterwards unless --keep-hf).
"""

import argparse
import json
import os
import shutil
import subprocess
import sys
import time


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--llama-cpp", required=True, help="llama.cpp source checkout (for convert_hf_to_gguf.py)")
    ap.add_argument("--out", required=True)
    ap.add_argument("--outtype", default="q8_0", choices=["q8_0", "f16", "bf16"])
    ap.add_argument("--work", default="", help="directory for the merged HF checkpoint (default: next to --out)")
    ap.add_argument("--keep-hf", action="store_true")
    ap.add_argument("--finalize-only", action="store_true", help="only rewrite the metadata of an existing --out")
    args = ap.parse_args()
    if args.finalize_only:
        finalize(os.path.abspath(os.path.expanduser(args.out)), args.llama_cpp)
        return 0

    import torch
    from kimodo.model.load_model import _build_local_text_encoder_conf
    from kimodo.model.llm2vec.llm2vec import LLM2Vec

    out = os.path.abspath(os.path.expanduser(args.out))
    work = os.path.abspath(os.path.expanduser(args.work)) if args.work else os.path.splitext(out)[0] + "_hf"
    conf = _build_local_text_encoder_conf()
    base = conf.get("base_model_name_or_path", "McGill-NLP/LLM2Vec-Meta-Llama-3-8B-Instruct-mntp")
    peft = conf.get("peft_model_name_or_path", "McGill-NLP/LLM2Vec-Meta-Llama-3-8B-Instruct-mntp-supervised")

    if not os.path.exists(os.path.join(work, "config.json")):
        t0 = time.time()
        l2v = LLM2Vec.from_pretrained(base, peft_model_name_or_path=peft, merge_peft=True,
                                      torch_dtype=torch.bfloat16, low_cpu_mem_usage=True)
        model = l2v.model
        if hasattr(model, "merge_and_unload"):
            model = model.merge_and_unload()
        print(f"merged {base} + {peft} in {time.time() - t0:.0f}s ({l2v.pooling_mode} pooling, "
              f"skip_instruction={l2v.skip_instruction})", flush=True)
        if hasattr(model, "_hf_peft_config_loaded"):   # merged: save plain weights, not an adapter
            model._hf_peft_config_loaded = False
        model.save_pretrained(work, safe_serialization=True, max_shard_size="4GB")
        l2v.tokenizer.save_pretrained(work)
        del model, l2v
        cfg_path = os.path.join(work, "config.json")
        cfg = json.load(open(cfg_path))
        cfg["architectures"] = ["LlamaBidirectionalModel"]   # llama.cpp llama-embed: no lm_head, non-causal
        json.dump(cfg, open(cfg_path, "w"), indent=2)

    conv = os.path.join(os.path.abspath(args.llama_cpp), "convert_hf_to_gguf.py")
    os.makedirs(os.path.dirname(out), exist_ok=True)
    subprocess.run([sys.executable, conv, work, "--outfile", out, "--outtype", args.outtype], check=True)
    finalize(out, args.llama_cpp)
    print(f"wrote {out} ({os.path.getsize(out) / 1e9:.2f} GB)")
    if not args.keep_hf:
        shutil.rmtree(work, ignore_errors=True)
    return 0


NAME = "Llama-3-LLM2Vec-8B-Kimodo-TextEncoder"


def finalize(path: str, llama_cpp: str) -> None:
    """Name (the Llama 3 licence requires derivative model names to start with "Llama 3"), licence fields, and
    the non-causal flag: llama.cpp defaults `causal_attn` to true when the key is absent, and LLM2Vec's
    bidirectional attention is what the embeddings were trained with."""
    sys.path.insert(0, os.path.join(os.path.abspath(llama_cpp), "gguf-py"))
    import gguf
    from gguf.scripts.gguf_new_metadata import MetadataDetails, copy_with_new_metadata
    reader = gguf.GGUFReader(path, "r")
    arch = reader.fields[gguf.Keys.General.ARCHITECTURE].contents()
    tmp = path + ".part"
    writer = gguf.GGUFWriter(tmp, arch=arch, endianess=reader.endianess)
    S, B = gguf.GGUFValueType.STRING, gguf.GGUFValueType.BOOL
    meta = {
        gguf.Keys.General.NAME: MetadataDetails(S, NAME),
        gguf.Keys.General.BASENAME: MetadataDetails(S, NAME),
        gguf.Keys.General.LICENSE: MetadataDetails(S, "llama3"),
        gguf.Keys.General.LICENSE_NAME: MetadataDetails(S, "Meta Llama 3 Community License"),
        gguf.Keys.General.LICENSE_LINK: MetadataDetails(S, "https://llama.meta.com/llama3/license"),
        gguf.Keys.General.DESCRIPTION: MetadataDetails(
            S, "Built with Meta Llama 3. Meta-Llama-3-8B-Instruct with the LLM2Vec MNTP + supervised LoRA merged "
               "(McGill-NLP), bidirectional; the text encoder of NVIDIA Kimodo, for QtMeshEditor."),
        gguf.Keys.Attention.CAUSAL.format(arch=arch): MetadataDetails(B, False),
    }
    copy_with_new_metadata(reader, writer, meta, [])
    del reader
    os.replace(tmp, path)


if __name__ == "__main__":
    sys.exit(main())
