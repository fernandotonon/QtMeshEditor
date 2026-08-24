#!/usr/bin/env python3
# ruff: noqa: E702, E741
"""Build the v6 text-to-motion training cache — CURATION-GRADE (#837).

ONE-TIME OFFLINE dev tool — NOT shipped. Successor to prep-t2m-v5.py.

The v5.1 model still rendered mushy walks: flow matching samples the
distribution it is given, and the v5 distribution contained everything the
extractor produced — turns, pauses, off-poses, style outliers. v6 applies
the SAME quality bar that curates the shipping template library to every
individual training window, folds the curated library takes themselves into
the set, and augments:

  gates (per window, canonical rep — d(f) = Q'(f)·D_c):
    - spine + neck/head upright (stricter for locomotion)
    - locomotion upper arms HANG, judged per arm on the SIGNED up-component
    - energy band: mean joint speed in [lo, hi] (poses and spasms both out)
  augmentation:
    - sagittal MIRROR: q' = (x, -y, -z, w) + swap L/R roles (doubles data,
      teaches symmetry)
    - SPEED 0.85x / 1.15x (slerp resample)

Windows are T=60 @ 30 fps (2 s). Output schema matches prep-t2m-v5
(mo/msk/tk/vocab/fps/canonRestDir) so train-t2m-flow-v5.py runs unchanged.

Usage:
  python3 scripts/prep-t2m-v6.py --corpus ~/motion_corpus \
      --bvh <cmu>/data --index <cmu>/cmu-mocap-index-text.txt \
      --library ~/t2m_v6/motion-library.json --out ~/t2m_v6/t2m_v6.npz
"""
import argparse
import importlib.util
import json
import os
import re
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))


def load_module(name, fname):
    spec = importlib.util.spec_from_file_location(name, os.path.join(HERE, fname))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


prep5 = load_module("prep5", "prep-t2m-v5.py")

J = 22
FPS = 30
D_CANON = prep5.D_CANON
# canonical L/R role pairs (AnimationMerger kLR)
LR = [(6, 10), (7, 11), (8, 12), (9, 13), (14, 18), (15, 19), (16, 20), (17, 21)]
MIRROR_PERM = list(range(J))
for a, b in LR:
    MIRROR_PERM[a], MIRROR_PERM[b] = b, a

LOCOMOTION = {"walk", "run", "march"}
HORIZONTAL_OK = {"death", "crawl", "roll", "swim", "fall", "sleep", "sit"}

# Source-exclusion (v6.2): some corpus characters carry a deliberately
# NON-HUMAN gait that the flow model faithfully reproduces on human prompts.
# Zombies shamble (bent spine, dragging/splayed stride, arms forward) and were
# 27% of the curated library incl. 5 walks + 1 run; "fruit"/produce characters
# (avocado etc.) are legless blobs whose canonical limbs are placeholders.
# Both poison locomotion. Matched case-insensitively against the clip's source
# string (asset title + animation name).
DEFAULT_EXCLUDE = r"zombie|avacado|avocado|fruit|banana|melon|undead|ghoul"


def qrot(q, v):
    qv = q[..., :3]
    uv = np.cross(qv, v)
    uuv = np.cross(qv, uv)
    return v + 2.0 * (q[..., 3:4] * uv + uuv)


def mean_dir_y(w, r):
    d = qrot(w[:, r], np.broadcast_to(D_CANON[r], (len(w), 3)))
    return float(d[:, 1].mean())


# canonical parent chain (AnimationMerger kParentCanon) for FK
PAR = [-1, 0, 1, 2, 3, 4, 2, 6, 7, 8, 2, 10, 11, 12, 0, 14, 15, 16, 0, 18, 19, 20]


def foot_travel_ratio(w):
    """fwd/side travel ratio of the feet over the window (unit bone lengths).
    A clean walk/run steps front-to-back (Z >> X); a splayed/side-step stride
    (the v6 model's failure mode) has X ~ Z. Positions come from the same
    canonical-direction FK the retarget uses, so this measures exactly what
    renders. Returns fwd/side (higher = cleaner)."""
    T = len(w)

    def pos(role):
        p = np.zeros((T, 3), np.float32)
        r = role
        while PAR[r] >= 0:
            p = p + qrot(w[:, PAR[r]], np.broadcast_to(D_CANON[r], (T, 3)))
            r = PAR[r]
        return p
    side = fwd = 0.0
    for foot in (17, 21):
        c = pos(foot)
        c = c - c.mean(0, keepdims=True)
        side += float(np.abs(c[:, 0]).mean())
        fwd += float(np.abs(c[:, 2]).mean())
    return fwd / (side + 1e-6)


def window_quality(action, w, valid):
    """True when the window meets the library curation bar."""
    # energy band — mean joint rotation speed (rad/frame)
    dq = np.abs((w[1:] * w[:-1]).sum(-1)).clip(0, 1)
    e = float((2 * np.arccos(dq)).mean())
    if not (0.004 <= e <= 0.11):
        return False
    if action in HORIZONTAL_OK:
        return True
    floor = 0.7 if action in LOCOMOTION else 0.5
    for r in (0, 1, 2):
        if valid[r]:
            if mean_dir_y(w, r) < floor:
                return False
            break
    for r in (3, 4, 5):
        if valid[r]:
            if mean_dir_y(w, r) < 0.5:
                return False
            break
    if action in LOCOMOTION:
        for r in (7, 11):
            if valid[r] and mean_dir_y(w, r) > -0.25:
                return False
        # Stride-directionality gate (v6.1): the feet must step FORWARD, not
        # sideways. 59% of raw CMU walk windows are splayed/side-stepping
        # (measured fwd/side < 1.5) — the model faithfully learned that
        # majority and walked sideways. Require a clean front-to-back stride.
        if valid[17] and valid[21] and foot_travel_ratio(w) < 2.0:
            return False
    return True


def mirror(w, valid):
    """Sagittal mirror: reflect each quat (x,-y,-z,w) and swap L/R roles."""
    m = w * np.array([1, -1, -1, 1], np.float32)
    return m[:, MIRROR_PERM], valid[MIRROR_PERM]


def retime(w, factor):
    """Slerp-resample a [T,J,4] window to the same length at `factor` speed."""
    T = w.shape[0]
    src = np.clip(np.arange(T, dtype=np.float64) * factor, 0, T - 1.001)
    i0 = src.astype(int)
    t = (src - i0)[:, None, None].astype(np.float32)
    a, b = w[i0], w[np.minimum(i0 + 1, T - 1)]
    # hemisphere-align then nlerp (windows are 30fps — angles tiny)
    dot = (a * b).sum(-1, keepdims=True)
    b = np.where(dot < 0, -b, b)
    out = a * (1 - t) + b * t
    return out / (np.linalg.norm(out, axis=-1, keepdims=True) + 1e-12)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--corpus", default="")
    ap.add_argument("--bvh", default="")
    ap.add_argument("--index", default="")
    ap.add_argument("--library", default="",
                    help="curated motion-library.json — takes are folded in "
                         "as extra (already-curated) source clips")
    ap.add_argument("--out", default=os.path.expanduser("~/t2m_v6/t2m_v6.npz"))
    ap.add_argument("--T", type=int, default=60)
    ap.add_argument("--min-roles", type=int, default=12)
    ap.add_argument("--min-action-windows", type=int, default=16)
    ap.add_argument("--exclude-sources", default=DEFAULT_EXCLUDE,
                    help="regex; clips whose source matches are DROPPED "
                         "(default: non-human gaits — zombies, produce). "
                         "Pass '' to disable.")
    a = ap.parse_args()

    excl = re.compile(a.exclude_sources, re.I) if a.exclude_sources else None
    dropped_src = [0]

    def excluded(src):
        if excl is None or not src or not excl.search(str(src)):
            return False
        dropped_src[0] += 1
        return True

    T = a.T
    mo, msk, acts = [], [], []
    gated = [0]

    def add(action, w, valid):
        mo.append(w)
        msk.append(valid)
        acts.append(action)

    def windows(action, cq, valid):
        nF = cq.shape[0]
        if nF < T // 2:
            return
        if nF < T:
            dq = np.abs((cq[-1] * cq[0]).sum(-1)).clip(0, 1)
            cyc = float((2 * np.arccos(dq)).mean()) < 0.25
            reps = [cq]
            while sum(r.shape[0] for r in reps) < T:
                reps.append(cq if cyc else cq[-1:].repeat(T, 0))
            cq = np.concatenate(reps, 0)[:T]
            nF = T
        for s in range(0, nF - T + 1, max(1, T // 3)):
            w = cq[s:s + T]
            if not window_quality(action, w, valid):
                gated[0] += 1
                continue
            add(action, w, valid)
            mw, mv = mirror(w, valid)
            add(action, mw, mv)
            for f in (0.85, 1.15):
                add(action, retime(w, f), valid)

    if a.corpus:
        n = 0
        for action, cq, valid, src in prep5.corpus_clips(
                os.path.expanduser(a.corpus), a.min_roles):
            if excluded(src):
                continue
            windows(action, cq, valid)
            n += 1
        print(f"corpus: {n} clips → {len(mo)} windows (cum)")
    if a.library:
        lib = json.load(open(os.path.expanduser(a.library)))
        n = 0
        for c in lib.get("clips", []):
            rw, rd = c.get("restWorld"), c.get("restDir")
            if not rw or not rd:
                continue
            if excluded(c.get("source", "")):
                continue
            cq, valid = prep5.canonicalize(
                np.asarray(c["quats"], np.float32), rw, rd)
            windows(c["action"], cq, valid)
            n += 1
        print(f"library: {n} takes → {len(mo)} windows (cum)")
    if a.bvh and a.index:
        n = 0
        for action, cq, valid, src in prep5.cmu_clips(a.bvh, a.index):
            if excluded(src):
                continue
            windows(action, cq, valid)
            n += 1
        print(f"cmu: {n} trials → {len(mo)} windows (cum)")

    print(f"quality gates dropped {gated[0]} base windows")
    print(f"source exclusion dropped {dropped_src[0]} clips "
          f"(pattern: {a.exclude_sources or None})")
    if not mo:
        sys.exit("no windows")
    from collections import Counter
    cnt = Counter(acts)
    vocab = sorted(w for w, k in cnt.items() if k >= a.min_action_windows)
    keep = [i for i, w in enumerate(acts) if w in vocab]
    mo = np.stack([mo[i] for i in keep]).astype(np.float32)
    msk = np.stack([msk[i] for i in keep]).astype(np.float32)
    tk = np.zeros((len(keep), len(vocab)), np.float32)
    for r, i in enumerate(keep):
        tk[r, vocab.index(acts[i])] = 1.0
    print(f"windows: {mo.shape[0]}  vocab({len(vocab)}):",
          {w: int(tk[:, vocab.index(w)].sum()) for w in vocab})
    os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)
    np.savez_compressed(a.out, mo=mo, msk=msk, tk=tk,
                        vocab=np.array(vocab), fps=FPS,
                        canonRestDir=D_CANON)
    print(f"wrote {a.out}  mo{mo.shape}")


if __name__ == "__main__":
    main()
