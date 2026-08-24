#!/usr/bin/env python3
# ruff: noqa: E702, E741
"""Score a t2m ONNX model on the POSTURE metrics that gate shipping (#837).

ONE-TIME OFFLINE dev tool — NOT shipped.

The v6/v6.1 quality bar was tracked by hand in ~/t2m_v6/EVAL_NOTES.md. This
script recomputes those exact numbers so any two models (and the training
data itself) are directly comparable:

  spineY   mean world up-component of the spine aim   (v6.1 shipped: 0.99)
  headY    ... of the neck/head aim                   (v6.1 shipped: 0.90)
  armY     signed up-component per upper arm, WORST   (v6.1 shipped: -0.98)
  fwd/side foot travel ratio, locomotion only         (v6.1 shipped: 2.29,
                                                       broken v6: 1.2, bar 2.0)

Metrics are computed on the model's own canonical output (the same quantity
prep-t2m-v6.py gates the training windows on), so "model vs data" is an
apples-to-apples read. Reports best-of-N per action the way the shipped
MotionGenerator picks a candidate, plus the mean, so sample VARIANCE (the
v6 walk failure mode) is visible rather than hidden by a lucky draw.

Usage:
  python3 scripts/eval-t2m-posture.py --model ~/t2m_v62/flow/t2m.onnx \
      --vocab ~/t2m_v62/flow/t2m-vocab.json [--data ~/t2m_v62/t2m_v62.npz] \
      [--actions walk,run,jump] [--samples 16]
"""
import argparse
import importlib.util
import json
import os

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))


def load_module(name, fname):
    spec = importlib.util.spec_from_file_location(name, os.path.join(HERE, fname))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


prep5 = load_module("prep5", "prep-t2m-v5.py")
prep6 = load_module("prep6", "prep-t2m-v6.py")

J = 22
D_CANON = prep5.D_CANON
LOCOMOTION = prep6.LOCOMOTION


def aim_y(w, roles):
    """Mean world up-component of the first VALID role's canonical aim."""
    for r in roles:
        return prep6.mean_dir_y(w, r)
    return float("nan")


def score_window(action, w):
    """Posture metrics for one [T,J,4] canonical window."""
    spine = aim_y(w, (0,))
    chest = aim_y(w, (2,))
    head = aim_y(w, (4,))
    arms = [prep6.mean_dir_y(w, r) for r in (7, 11)]
    out = {
        "spineY": spine,
        "chestY": chest,
        "headY": head,
        "armY_worst": max(arms),   # arms hang => strongly negative; worst = least negative
        "armY_L": arms[0],
        "armY_R": arms[1],
    }
    if action in LOCOMOTION:
        out["fwd_side"] = prep6.foot_travel_ratio(w)
    return out


def quat_from_motion(motion):
    """MotionGenerator output [T,220] -> canonical quats [T,J,4] (x,y,z,w)."""
    m = np.asarray(motion, np.float32).reshape(motion.shape[0], J, 10)
    q = m[:, :, 3:7]
    return q / (np.linalg.norm(q, axis=-1, keepdims=True) + 1e-12)


def fmt(d):
    keys = ["spineY", "chestY", "headY", "armY_worst", "fwd_side"]
    return "  ".join(f"{k}={d[k]:+.3f}" for k in keys if k in d)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--model", required=True)
    ap.add_argument("--vocab", default="")
    ap.add_argument("--data", default="", help="npz cache — also score the DATA")
    ap.add_argument("--actions", default="", help="comma list; default = all vocab")
    ap.add_argument("--samples", type=int, default=16)
    ap.add_argument("--seed", type=int, default=0)
    a = ap.parse_args()

    vocab_path = a.vocab or os.path.join(os.path.dirname(a.model), "t2m-vocab.json")
    vj = json.load(open(os.path.expanduser(vocab_path)))
    vocab = vj["vocab"] if isinstance(vj, dict) and "vocab" in vj else vj
    if isinstance(vocab, dict):
        vocab = vocab.get("actions", [])
    print(f"vocab({len(vocab)}): {vocab}")

    import onnxruntime as ort
    so = ort.SessionOptions()
    so.log_severity_level = 3
    sess = ort.InferenceSession(os.path.expanduser(a.model), so,
                                providers=["CPUExecutionProvider"])
    inp = {i.name: i.shape for i in sess.get_inputs()}
    print("model inputs:", inp)
    tok_name = next(n for n in inp if "tok" in n.lower())
    seed_name = next((n for n in inp if "seed" in n.lower()), None)
    zdim = None
    if seed_name is not None:
        zdim = int(inp[seed_name][-1])

    actions = [s for s in a.actions.split(",") if s] or list(vocab)
    rng = np.random.default_rng(a.seed)

    # ---- data reference (what the model is trying to match) ----
    if a.data:
        z = np.load(os.path.expanduser(a.data), allow_pickle=True)
        mo, tk = z["mo"], z["tk"]
        dvocab = [str(s) for s in z["vocab"]]
        print("\n=== TRAINING DATA (reference) ===")
        for act in actions:
            if act not in dvocab:
                continue
            idx = np.nonzero(tk[:, dvocab.index(act)])[0]
            if not len(idx):
                continue
            pick = idx[:64]
            ss = [score_window(act, mo[i]) for i in pick]
            agg = {k: float(np.mean([s[k] for s in ss])) for k in ss[0]}
            print(f"  {act:10s} n={len(idx):6d}  {fmt(agg)}")

    # ---- model ----
    print(f"\n=== MODEL ({a.samples} samples/action) ===")
    print(f"{'action':10s} {'best-of-N':>44s} | {'mean':>44s}")
    for act in actions:
        if act not in vocab:
            print(f"  {act:10s} NOT IN VOCAB")
            continue
        t = np.zeros((1, len(vocab)), np.float32)
        t[0, vocab.index(act)] = 1.0
        scores = []
        for _ in range(a.samples):
            feeds = {tok_name: t}
            if seed_name is not None:
                feeds[seed_name] = (rng.standard_normal((1, zdim)) * 0.5).astype(np.float32)
            out = sess.run(None, feeds)[0][0]
            scores.append(score_window(act, quat_from_motion(out)))
        # rank the way the shipped scorer does: upright + arms hanging
        def rank(s):
            r = s["spineY"] + s["headY"] - s["armY_worst"]
            if "fwd_side" in s:
                r += min(s["fwd_side"], 4.0)
            return r
        best = max(scores, key=rank)
        mean = {k: float(np.mean([s[k] for s in scores])) for k in scores[0]}
        print(f"  {act:10s} {fmt(best)} | {fmt(mean)}")


if __name__ == "__main__":
    main()
