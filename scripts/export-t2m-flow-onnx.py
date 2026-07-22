#!/usr/bin/env python3
"""Export a trained t2m flow model (flow.pt) to ONNX + vocab json.

Split out of train-t2m-flow-v5.py so the export can be re-run after training
without repeating the (long) training loop, and to stay compatible with older
torch (torch.onnx.export gained the `dynamo=` kwarg in 2.5 — 2.2.x rejects it).

USAGE
  python3 scripts/export-t2m-flow-onnx.py --run <out_dir> --data <npz>
      [--dim 256 --layers 6 --steps 16 --guidance 1.0]
where <out_dir> holds flow.pt from train-t2m-flow-v5.py and <npz> is its data.
"""
import argparse
import importlib.util
import inspect
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


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--run", required=True, help="dir with flow.pt")
    ap.add_argument("--data", required=True, help="training npz (for T/V/fps/canonRestDir)")
    ap.add_argument("--dim", type=int, default=256)
    ap.add_argument("--layers", type=int, default=6)
    ap.add_argument("--steps", type=int, default=16)
    ap.add_argument("--guidance", type=float, default=1.0)
    a = ap.parse_args()

    tr = load_module("trainflow", "train-t2m-flow-v5.py")
    FlowDiT, Sampler = tr.FlowDiT, tr.Sampler
    J, C6, D6 = tr.J, tr.C6, tr.D6

    d = np.load(a.data, allow_pickle=True)
    vocab = [str(w) for w in d["vocab"]]
    T = int(d["mo"].shape[1])
    V = len(vocab)
    fps = int(d["fps"])
    canon_rd = d["canonRestDir"]
    C = J * 10

    net = FlowDiT(V, T, dim=a.dim, layers=a.layers)
    sd = torch.load(os.path.join(a.run, "flow.pt"), map_location="cpu",
                    weights_only=True)
    net.load_state_dict(sd)
    net.eval()

    samp = Sampler(net, V, T, a.steps, a.guidance).eval()
    Z = T * C6
    tokens = torch.zeros(1, V); tokens[0, 0] = 1.0
    seed = torch.randn(1, Z) * 0.5
    onnx_path = os.path.join(a.run, "t2m.onnx")

    # torch.onnx.export gained `dynamo` in 2.5; pass it only if supported.
    kw = dict(input_names=["tokens", "seed"], output_names=["motion"],
              opset_version=17)
    if "dynamo" in inspect.signature(torch.onnx.export).parameters:
        kw["dynamo"] = False
    torch.onnx.export(samp, (tokens, seed), onnx_path, **kw)

    vj = {
        "vocab": vocab, "Z": Z, "T": T, "C": C, "J": J,
        "fps": fps, "frame": "world", "version": "v5-flow-100style",
        "flowSteps": a.steps,
        "restWorld": [[0.0, 0.0, 0.0, 1.0]] * J,
        "restDir": [[float(v) for v in row] for row in canon_rd],
    }
    with open(os.path.join(a.run, "t2m-vocab.json"), "w") as f:
        json.dump(vj, f)
    print(f"exported {onnx_path} ({os.path.getsize(onnx_path) / 1e6:.1f} MB) + vocab")

    # sanity via onnxruntime
    import onnxruntime as ort
    s = ort.InferenceSession(onnx_path, providers=["CPUExecutionProvider"])
    out = s.run(None, {"tokens": tokens.numpy(), "seed": seed.numpy()})[0]
    nrm = np.linalg.norm(out[0].reshape(T, J, 10)[..., 3:7], axis=-1)
    print("onnx ok:", out.shape, "quat norms",
          round(float(nrm.min()), 4), round(float(nrm.max()), 4))


if __name__ == "__main__":
    main()
