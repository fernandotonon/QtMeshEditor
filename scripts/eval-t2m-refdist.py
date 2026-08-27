#!/usr/bin/env python3
# ruff: noqa: E702, E741
"""Score a t2m model by DISTANCE TO REAL REFERENCE CLIPS (#837).

ONE-TIME OFFLINE dev tool — NOT shipped.

Why this exists. The hand-designed property metrics (travel direction,
contralateral phase, torso twist, limb amplitude, joint speed, gait
periodicity) can each be driven to match real motion while the render still
collapses: six independent hinges leave corners that satisfy all of them and
still look wrong, and five of those metrics turned out to have bugs that made
them reward the very defect they were added to catch.

Real motion is a joint distribution, so compare against it directly. For each
generated clip, find its nearest real reference window in the canonical
representation and report that distance. One number, computed against ground
truth, that cannot be gamed by satisfying a property in isolation.

Distance is per-frame mean geodesic quaternion angle over the valid canonical
joints, minimised over reference windows AND over a cyclic time shift of the
generated clip (phase is not a defect, so it should not be penalised).

Usage:
  python3 scripts/eval-t2m-refdist.py --model ~/t2m_v76/flow \
      [--refs "Walk.fbx=walk,Running.fbx=run"] [--samples 12]
"""
import argparse
import importlib.util
import json
import os
import subprocess
import tempfile

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
DOWNLOADS = os.path.expanduser("~/Downloads")


def load_module(name, fname):
    spec = importlib.util.spec_from_file_location(name, os.path.join(HERE, fname))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


prep5 = load_module("prep5", "prep-t2m-v5.py")

J = 22


def canon_from_fbx(path, cache_dir):
    """Dump a real clip to canonical quats via the shipping extractor."""
    base = os.path.basename(path).replace(" ", "_") + ".canon.json"
    out = os.path.join(cache_dir, base)
    if not os.path.exists(out):
        subprocess.run(["qtmesh", "anim", path, "--dump-canonical", out],
                       capture_output=True, text=True)
    if not os.path.exists(out):
        return None
    d = json.load(open(out))
    clips = d.get("clips", [])
    if not clips:
        return None
    c = clips[0]
    cq, valid = prep5.canonicalize(np.asarray(c["quats"], np.float32),
                                   c["restWorld"], c["restDir"])
    return cq, valid


def windows_of(cq, T=60, stride=5):
    """Reference windows, cycle-extending short clips.

    The real clips are short — Walk.fbx is exactly 2.0 s (one 60-frame window)
    and Running.fbx only 0.7 s — so a plain slice yields one or zero windows and
    the distance has almost nothing to match against. Loop the clip and take
    phase-shifted windows so every phase of the cycle is represented.
    """
    n = len(cq)
    if n < 2:
        return []
    reps = int(np.ceil((T * 2) / n)) + 1
    ext = np.concatenate([cq] * reps, axis=0)
    outs = []
    limit = min(len(ext) - T, max(n, T))
    for s in range(0, limit + 1, stride):
        outs.append(ext[s:s + T])
    return outs


def geodesic_dist(a, b, valid):
    """Mean per-frame per-joint geodesic angle (radians) between two clips."""
    dot = np.abs((a * b).sum(-1)).clip(0.0, 1.0)
    ang = 2.0 * np.arccos(dot)                     # [T,J]
    m = valid[None, :]
    return float((ang * m).sum() / max(m.sum() * ang.shape[0], 1e-6))


def best_distance(gen, refs, valid):
    """Min over reference windows and over cyclic shifts of `gen`."""
    T = len(gen)
    best = float("inf")
    for r in refs:
        for sh in range(0, T, 4):
            g = np.roll(gen, sh, axis=0)
            d = geodesic_dist(g, r, valid)
            if d < best:
                best = d
    return best


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--model", required=True, help="dir with t2m.onnx + vocab")
    ap.add_argument("--refs", default="Walk.fbx=walk,Running.fbx=run")
    ap.add_argument("--samples", type=int, default=12)
    ap.add_argument("--seed", type=int, default=11)
    a = ap.parse_args()

    cache = os.path.join(tempfile.gettempdir(), "t2m_refcache")
    os.makedirs(cache, exist_ok=True)

    refs = {}
    valid_all = np.ones(J, np.float32)
    for spec in a.refs.split(","):
        if "=" not in spec:
            continue
        fname, action = spec.split("=", 1)
        path = os.path.join(DOWNLOADS, fname.strip())
        if not os.path.exists(path):
            print(f"  (missing reference {fname})")
            continue
        got = canon_from_fbx(path, cache)
        if got is None:
            print(f"  (could not extract {fname})")
            continue
        cq, valid = got
        refs.setdefault(action.strip(), []).extend(windows_of(cq))
        valid_all = np.minimum(valid_all, valid)
    if not refs:
        raise SystemExit("no references extracted")
    print(f"references: " + ", ".join(f"{k}({len(v)} windows)"
                                      for k, v in refs.items()))

    mp = os.path.expanduser(a.model)
    import onnxruntime as ort
    so = ort.SessionOptions()
    so.log_severity_level = 3
    sess = ort.InferenceSession(os.path.join(mp, "t2m.onnx"), so,
                                providers=["CPUExecutionProvider"])
    vocab = json.load(open(os.path.join(mp, "t2m-vocab.json")))["vocab"]
    zd = int(sess.get_inputs()[1].shape[-1])
    rng = np.random.default_rng(a.seed)

    # self-distance floor: how close do DIFFERENT real windows get to each
    # other? That is the best any model could plausibly score.
    for act, rws in refs.items():
        if len(rws) > 1:
            ds = [geodesic_dist(rws[i], rws[j], valid_all)
                  for i in range(len(rws)) for j in range(len(rws)) if i != j]
            print(f"  {act}: real-vs-real distance min {min(ds):.4f} "
                  f"median {np.median(ds):.4f} rad")

    print(f"\n{'action':8s} {'refDist(best)':>14s} {'refDist(mean)':>14s}")
    for act, rws in refs.items():
        if act not in vocab:
            print(f"  {act:8s} not in vocab")
            continue
        t = np.zeros((1, len(vocab)), np.float32)
        t[0, vocab.index(act)] = 1.0
        ds = []
        for _ in range(a.samples):
            seed = (rng.standard_normal((1, zd)) * 0.5).astype(np.float32)
            out = sess.run(None, {"tokens": t, "seed": seed})[0][0]
            q = out.reshape(-1, J, 10)[:, :, 3:7]
            q = q / (np.linalg.norm(q, axis=-1, keepdims=True) + 1e-12)
            ds.append(best_distance(q, rws, valid_all))
        print(f"  {act:8s} {min(ds):14.4f} {np.mean(ds):14.4f}   rad "
              f"({np.degrees(np.mean(ds)):.1f} deg/joint)")


if __name__ == "__main__":
    main()
