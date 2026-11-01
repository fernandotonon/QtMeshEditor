#!/usr/bin/env python3
"""Weight-only quantization of Kimodo's LLM2Vec-Llama-3-8B text encoder.

Kimodo conditions on LLM2Vec embeddings of Meta-Llama-3-8B-Instruct, which
needs ~16 GB in bfloat16: on a 24 GB Apple Silicon machine that is most of the
unified memory, and reading it takes minutes. This script builds the encoder
once (base + MNTP + supervised LoRA, merged), quantizes every nn.Linear of
the transformer to int8 or int4 (symmetric, one scale per output row and group of
128 input columns: a per-row int8 scale measured cosine 0.94 against bf16 on
Llama 3, whose outlier input channels inflate a whole row's scale), and saves the result as a LOCAL checkpoint the sidecar loads
instead (`generate.py --text-encoder-quant int8|int4`).

The checkpoint is a derivative of Meta Llama 3 built from the user's own gated
download. It stays on the user's machine (Meta Llama 3 Community License): it
is never bundled, mirrored or uploaded by QtMeshEditor.

No new dependency: dequantization is a plain PyTorch op, so it runs on MPS,
CUDA and CPU alike (bitsandbytes has no MPS backend).

    python textenc_quant.py --bits 8 --out <AppData>/kimodo/textenc_int8
"""

from __future__ import annotations

import argparse
import gc
import json
import os
import sys
import time

FORMAT = "qtmesh-kimodo-textenc-quant-v2"
GROUP = 128   # int4 group size (columns)


def log(msg: str) -> None:
    print(msg, file=sys.stderr, flush=True)


def _torch():
    import torch
    return torch


def _quant_linear_class():
    torch = _torch()
    nn = torch.nn

    class _QuantLinear(nn.Module):
        """y = x @ dequant(W)^T + b. W is int8 or packed int4, one float16 scale per row and 128-column group."""

        def __init__(self, in_features: int, out_features: int, bits: int, bias: bool, device=None):
            super().__init__()
            self.in_features, self.out_features, self.bits = in_features, out_features, bits
            groups = (in_features + GROUP - 1) // GROUP
            if bits == 8:
                self.register_buffer("qweight", torch.empty(out_features, groups * GROUP, dtype=torch.int8, device=device))
            else:
                self.register_buffer("qweight", torch.empty(out_features, groups * GROUP // 2, dtype=torch.uint8, device=device))
            self.register_buffer("scale", torch.empty(out_features, groups, dtype=torch.float16, device=device))
            if bias:
                self.register_buffer("bias", torch.empty(out_features, dtype=torch.bfloat16, device=device))
            else:
                self.bias = None

        @staticmethod
        def quantize(weight, bits: int):
            """weight [out, in] (any float dtype) -> (qweight, scale)."""
            w = weight.detach().float()
            out, inn = w.shape
            groups = (inn + GROUP - 1) // GROUP
            pad = groups * GROUP - inn
            if pad:
                w = torch.nn.functional.pad(w, (0, pad))
            wg = w.view(out, groups, GROUP)
            if bits == 8:
                scale = (wg.abs().amax(dim=2, keepdim=True) / 127.0).clamp_min(1e-12)
                q = torch.round(wg / scale).clamp(-127, 127).to(torch.int8)
                return q.view(out, groups * GROUP), scale.view(out, groups).half()
            scale = (wg.abs().amax(dim=2, keepdim=True) / 7.0).clamp_min(1e-12)
            q = torch.round(wg / scale).clamp(-8, 7).to(torch.int16) + 8   # 0..15
            q = q.view(out, groups * GROUP).to(torch.uint8)
            packed = (q[:, 0::2] | (q[:, 1::2] << 4)).contiguous()
            return packed, scale.view(out, groups).half()

        def weight_as(self, dtype):
            groups = self.scale.shape[1]
            if self.bits == 8:
                q = self.qweight
            else:
                lo = (self.qweight & 0x0F).to(torch.int16)
                hi = (self.qweight >> 4).to(torch.int16)
                q = torch.stack((lo, hi), dim=2).view(self.out_features, -1) - 8
            w = q.view(self.out_features, groups, GROUP).to(dtype) * self.scale.to(dtype).unsqueeze(2)
            return w.view(self.out_features, groups * GROUP)[:, : self.in_features]

        def forward(self, x):
            w = self.weight_as(x.dtype)
            b = None if self.bias is None else self.bias.to(x.dtype)
            return torch.nn.functional.linear(x, w, b)

        def extra_repr(self) -> str:
            return f"in={self.in_features}, out={self.out_features}, bits={self.bits}"

    return _QuantLinear


def _replace_linears(module, bits: int, quantize: bool, device=None) -> int:
    """Swap every nn.Linear under `module` for a QuantLinear. With `quantize`, fills it from the Linear's weights."""
    torch = _torch()
    QL = _quant_linear_class()
    count = 0
    for name, child in list(module.named_children()):
        if isinstance(child, torch.nn.Linear):
            ql = QL(child.in_features, child.out_features, bits, child.bias is not None, device=device)
            if quantize:
                q, s = QL.quantize(child.weight, bits)
                ql.qweight, ql.scale = q, s
                if child.bias is not None:
                    ql.bias = child.bias.detach().to(torch.bfloat16)
            setattr(module, name, ql)
            count += 1
        else:
            count += _replace_linears(child, bits, quantize, device)
    return count


def build_and_save(out_dir: str, bits: int) -> dict:
    """Load the bf16 encoder on the CPU, merge the LoRA, quantize, and write `out_dir`."""
    torch = _torch()
    from safetensors.torch import save_file
    from kimodo.model.load_model import _build_local_text_encoder_conf, instantiate_from_dict

    conf = _build_local_text_encoder_conf()
    t0 = time.time()
    prev = os.environ.get("TEXT_ENCODER_DEVICE")
    os.environ["TEXT_ENCODER_DEVICE"] = "cpu"   # quantize in RAM, not in unified GPU memory
    try:
        enc = instantiate_from_dict(conf)
    finally:
        if prev is None:
            os.environ.pop("TEXT_ENCODER_DEVICE", None)
        else:
            os.environ["TEXT_ENCODER_DEVICE"] = prev
    log(f"loaded the bf16 encoder in {time.time() - t0:.0f}s")
    l2v = enc.model
    model = l2v.model
    if hasattr(model, "merge_and_unload"):   # PeftModel: fold the supervised LoRA into the base weights
        model = model.merge_and_unload()
    n = _replace_linears(model, bits, quantize=True)
    gc.collect()
    log(f"quantized {n} linear layers to int{bits}")

    os.makedirs(out_dir, exist_ok=True)
    state = {k: v.contiguous() for k, v in model.state_dict().items()}
    tmp = os.path.join(out_dir, "model.safetensors.part")
    save_file(state, tmp)
    os.replace(tmp, os.path.join(out_dir, "model.safetensors"))
    model.config.save_pretrained(out_dir)
    l2v.tokenizer.save_pretrained(out_dir)
    meta = {
        "format": FORMAT,
        "bits": bits,
        "group": GROUP,
        "llm_dim": int(enc.llm_dim),
        "name_or_path": model.config._name_or_path,
        "pooling_mode": l2v.pooling_mode,
        "max_length": l2v.max_length,
        "doc_max_length": l2v.doc_max_length,
        "skip_instruction": l2v.skip_instruction,
        "base": conf["base_model_name_or_path"],
        "peft": conf["peft_model_name_or_path"],
        "licence": "Derivative of Meta Llama 3 (Meta Llama 3 Community License). Local use only; do not redistribute.",
    }
    with open(os.path.join(out_dir, "quant.json"), "w") as f:
        json.dump(meta, f, indent=2)
    size = os.path.getsize(os.path.join(out_dir, "model.safetensors"))
    log(f"wrote {out_dir} ({size / 1e9:.2f} GB) in {time.time() - t0:.0f}s")
    return meta


def is_valid_dir(path: str, bits: int | None = None) -> bool:
    try:
        with open(os.path.join(path, "quant.json")) as f:
            meta = json.load(f)
    except (OSError, ValueError):
        return False
    return (meta.get("format") == FORMAT and os.path.isfile(os.path.join(path, "model.safetensors"))
            and (bits is None or meta.get("bits") == bits))


class QuantizedEncoder:
    """Drop-in for kimodo's LLM2VecEncoder (same __call__ contract) backed by a quantized checkpoint."""

    def __init__(self, path: str, device: str):
        torch = _torch()
        from safetensors.torch import load_file
        from transformers import AutoConfig, AutoTokenizer
        from kimodo.model.llm2vec.llm2vec import LLM2Vec
        from kimodo.model.llm2vec.models.bidirectional_llama import LlamaBiModel

        with open(os.path.join(path, "quant.json")) as f:
            meta = json.load(f)
        if meta.get("format") != FORMAT:
            raise ValueError(f"{path}: not a {FORMAT} checkpoint")
        config = AutoConfig.from_pretrained(path)
        with torch.device("meta"):
            model = LlamaBiModel(config)
        _replace_linears(model, int(meta["bits"]), quantize=False, device="meta")
        state = load_file(os.path.join(path, "model.safetensors"), device="cpu")
        missing, unexpected = model.load_state_dict(state, strict=False, assign=True)
        if unexpected:
            raise ValueError(f"{path}: unexpected tensors {unexpected[:5]}")
        # Non-persistent buffers (rotary frequencies) are not in the file: rebuild them for real.
        if hasattr(model, "rotary_emb"):
            model.rotary_emb = type(model.rotary_emb)(config=config)
        # LLM2Vec picks the prompt template from this exact string (Llama 3 Instruct gets its chat header); a
        # checkpoint saved to a folder would otherwise carry the folder name and silently encode WITHOUT the
        # template (measured: cosine 0.94 against the reference embeddings with identical weights).
        model.config._name_or_path = meta["name_or_path"]
        still_meta = [n for n, t in list(model.named_parameters()) + list(model.named_buffers()) if t.is_meta]
        if still_meta or [m for m in missing if "rotary" not in m]:
            raise ValueError(f"{path}: incomplete checkpoint ({(still_meta or missing)[:5]})")
        model.eval()
        tokenizer = AutoTokenizer.from_pretrained(path)
        tokenizer.pad_token = tokenizer.eos_token
        tokenizer.padding_side = "left"
        self.model = LLM2Vec(model, tokenizer, pooling_mode=meta["pooling_mode"], max_length=meta["max_length"],
                             doc_max_length=meta["doc_max_length"], skip_instruction=meta["skip_instruction"])
        self.model = self.model.to(device)
        self.llm_dim = int(meta["llm_dim"])
        self.bits = int(meta["bits"])
        self._device = device

    def __call__(self, text):
        torch = _torch()
        texts = [text] if isinstance(text, str) else list(text)
        with torch.no_grad():
            enc = self.model.encode(texts, batch_size=1, show_progress_bar=False, device=self._device)
        enc = (enc if isinstance(enc, torch.Tensor) else torch.as_tensor(enc))[:, None].to(self._device)
        return enc, [1] * len(texts)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--bits", type=int, choices=(4, 8), default=8)
    ap.add_argument("--out", required=True, help="output directory (e.g. <AppData>/kimodo/textenc_int8)")
    args = ap.parse_args()
    try:
        build_and_save(args.out, args.bits)
    except Exception as e:  # gated access, out of memory, ...
        log(f"error: {e}")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
