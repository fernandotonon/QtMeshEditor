#!/usr/bin/env python3
# ruff: noqa: E702, E741
"""Export a t2m ONNX from a MID-RUN training checkpoint (#837).

ONE-TIME OFFLINE dev tool — NOT shipped.

train-t2m-flow-v5.py only exports ONNX after its LAST epoch, so a 400-epoch
(~9h) run is unobservable until it finishes. This reuses the trainer's own
FlowDiT/Sampler definitions and export block against `<out>/ckpt.pt`, so a
run in progress can be scored with eval-t2m-posture.py at any milestone
(the v6 notes show walk quality still climbing at ep200 → ep400, which is
exactly what you want to watch rather than assume).

Reads the arch dims from the checkpoint's own tensor shapes, so it does not
need to be told --dim/--layers.

Usage:
  python3 scripts/export-t2m-from-ckpt.py --ckpt ~/t2m_v62/flow/ckpt.pt \
      --data ~/t2m_v62/t2m_v62.npz --out ~/t2m_v62/eval_ep100 [--steps 24]
"""
import argparse
import importlib.util
import json
import os

import numpy as np
import torch

HERE = os.path.dirname(os.path.abspath(__file__))


def load_module(name, fname):
    spec = importlib.util.spec_from_file_location(name, os.path.join(HERE, fname))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


tr = load_module("tr", "train-t2m-flow-v5.py")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ckpt", required=True)
    ap.add_argument("--data", required=True, help="npz (for vocab + canonRestDir)")
    ap.add_argument("--out", required=True)
    ap.add_argument("--steps", type=int, default=24)
    # 1.0 = the value the SHIPPED v8.0 model was exported with. A mismatch
    # here silently changes generated motion and invalidates checkpoint
    # comparisons against it, so keep all three defaults (this, the trainer
    # flag, and Sampler.__init__) on 1.0.
    ap.add_argument("--guidance", type=float, default=1.0)
    a = ap.parse_args()

    z = np.load(os.path.expanduser(a.data), allow_pickle=False)
    vocab = [str(s) for s in z["vocab"]]
    fps = int(z["fps"]) if "fps" in z else 30
    canon_rd = z["canonRestDir"]
    T = int(z["mo"].shape[1])
    V = len(vocab)

    ck = torch.load(os.path.expanduser(a.ckpt), map_location="cpu",
                    weights_only=True)
    sd = ck.get("net", ck)
    epoch = ck.get("epoch", "?")

    # infer arch from tensor shapes rather than trusting flags
    dim = None
    for k, v in sd.items():
        if k.endswith("inp.weight") or ("inp" in k and v.ndim == 2):
            dim = v.shape[0]
            break
    if dim is None:
        dim = next(v.shape[-1] for v in sd.values() if v.ndim == 2)
    layers = 1 + max(
        (int(k.split(".")[1]) for k in sd if k.startswith("blocks.")),
        default=0)
    print(f"ckpt epoch={epoch}  dim={dim} layers={layers} T={T} V={V}")

    net = tr.FlowDiT(V, T, dim=dim, layers=layers)
    net.load_state_dict(sd)
    net.eval()

    samp = tr.Sampler(net, V, T, a.steps, a.guidance).eval()
    C6 = tr.C6
    J = tr.J
    Z = T * C6
    tokens = torch.zeros(1, V); tokens[0, 0] = 1.0
    seed = torch.randn(1, Z) * 0.5

    os.makedirs(os.path.expanduser(a.out), exist_ok=True)
    onnx_path = os.path.join(os.path.expanduser(a.out), "t2m.onnx")
    torch.onnx.export(samp, (tokens, seed), onnx_path,
                      input_names=["tokens", "seed"],
                      output_names=["motion"], opset_version=17,
                      dynamo=False)
    vj = {
        "vocab": vocab, "Z": Z, "T": T, "C": J * 10, "J": J,
        "fps": fps, "frame": "world", "version": f"v5-flow-ep{epoch}",
        "flowSteps": a.steps,
        "restWorld": [[0.0, 0.0, 0.0, 1.0]] * J,
        "restDir": [[float(v) for v in row] for row in canon_rd],
    }
    with open(os.path.join(os.path.expanduser(a.out), "t2m-vocab.json"), "w") as f:
        json.dump(vj, f)
    print(f"exported {onnx_path} ({os.path.getsize(onnx_path)/1e6:.1f} MB) "
          f"+ vocab (epoch {epoch})")


if __name__ == "__main__":
    main()
