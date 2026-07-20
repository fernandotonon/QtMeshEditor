#!/usr/bin/env python3
# ruff: noqa: E702, E741
"""Build the v5 text-to-motion training cache from the motion corpus (#858).

ONE-TIME OFFLINE dev tool — NOT shipped. Successor to prep-t2m-v4.py, fixing
its structural flaw: v4 trained on raw per-rig world quats, so every rig's
bone-axis convention leaked into the data (the "convention mush" that made
the CVAE average modes into gentle motion). v5 CANONICALIZES every clip into
a rig-independent representation before windowing:

  For each canonical joint c with source reference triple (restWorld Q_r,
  restDir d_r) — the same triple the bind-referenced retarget (PR #843)
  consumes — the source bone's world direction trajectory is
      d(f) = W(f) · (Q_r⁻¹ · d̂_r)                     (canonical axes)
  and its roll about the bone is the swing/twist residual
      Δ(f)  = W(f) · Q_r⁻¹
      aim   = arc(d̂_r → d(f))
      θ(f)  = signed angle of (aim⁻¹ · Δ(f)) about d̂_r, unwrapped over f.
  The training quat is rebuilt against a FIXED canonical T-pose direction
  set D (spine +Y, left arm +X, right arm −X, legs −Y, feet +Z):
      Q'(f) = twist(θ(f), d(f)) · arc(D_c → d(f))

  Q' is identical for every rig performing the same motion — and it is
  EXACTLY what the runtime consumes: with the model's reference triple
  (restWorld = identity, restDir = D), applyMotionClip recovers
  d(f) = Q'(f)·D_c and (with #857 twist transport) θ(f) losslessly. The
  model therefore rides the SAME retarget path as v5 template clips, no
  synthetic-standing-pose shim.

Sources:
  --corpus   ~/motion_corpus: the *.canonical.json sidecar dumps written by
             build-motion-library-v5.py / `qtmesh anim --dump-canonical`
             (#838/#839). Labels via the SAME action keyword table as the
             template library, so both motion sources cover the same vocab.
  --bvh      CMU BVH conversion (optional): FK world quats (rest = identity,
             restDir from the skeleton's rest joint positions), labels from
             the public CMU index — the v4 path, canonicalized the same way.

Output npz: mo[N,T,22,4] canonical quats, msk[N,22] per-joint validity,
tk[N,V] one-hot, vocab, fps, canonRestDir[22,3] (= D, for the vocab json).

Usage:
  python3 scripts/prep-t2m-v5.py --corpus ~/motion_corpus \
      [--bvh /tmp/cmu-mocap-bvh/data --index /tmp/cmu_index.txt] \
      --out /tmp/t2m_v5.npz [--T 60]
"""
import argparse
import glob
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


libv5 = load_module("libv5", "build-motion-library-v5.py")   # action_for, quality
prep4 = load_module("prep4", "prep-t2m-v4.py")               # BVH FK, index, windows

CANON = prep4.CANON
J = 22
FPS = 30
# canonical chain parent (AnimationMerger kParentCanon)
PARENT = [-1, 0, 1, 2, 3, 4, 2, 6, 7, 8, 2, 10, 11, 12, 0, 14, 15, 16, 0, 18, 19, 20]
CHILD = {}
for _c, _p in enumerate(PARENT):     # chain child = FIRST child (hip→abdomen,
    if _p >= 0:                      # chest→neck), matching the extractor
        CHILD.setdefault(_p, _c)

# FIXED canonical T-pose bone directions D (canonical axes: +Y up, +Z fwd,
# +X left — the CMU frame convention every dump is normalized into).
D_CANON = np.array([
    [0, 1, 0],  [0, 1, 0],  [0, 1, 0],  [0, 1, 0],  [0, 1, 0],  [0, 1, 0],
    [-1, 0, 0], [-1, 0, 0], [-1, 0, 0], [-1, 0, 0],
    [1, 0, 0],  [1, 0, 0],  [1, 0, 0],  [1, 0, 0],
    [0, -1, 0], [0, -1, 0], [0, -1, 0], [0, 0, 1],
    [0, -1, 0], [0, -1, 0], [0, -1, 0], [0, 0, 1],
], np.float32)


# ---------- quat math, (x,y,z,w), vectorised over leading dims ----------
def qmul(a, b):
    ax, ay, az, aw = np.moveaxis(a, -1, 0)
    bx, by, bz, bw = np.moveaxis(b, -1, 0)
    return np.stack([aw*bx + ax*bw + ay*bz - az*by,
                     aw*by - ax*bz + ay*bw + az*bx,
                     aw*bz + ax*by - ay*bx + az*bw,
                     aw*bw - ax*bx - ay*by - az*bz], -1)


def qconj(q):
    return q * np.array([-1, -1, -1, 1], q.dtype)


def qrot(q, v):
    """Rotate vectors v[...,3] by quats q[...,4]."""
    qv = q[..., :3]
    uv = np.cross(qv, v)
    uuv = np.cross(qv, uv)
    return v + 2.0 * (q[..., 3:4] * uv + uuv)


def qnorm(q):
    return q / (np.linalg.norm(q, axis=-1, keepdims=True) + 1e-12)


def arc(a, b):
    """Shortest-arc quats rotating unit vectors a[...,3] onto b[...,3]."""
    d = (a * b).sum(-1, keepdims=True)
    axis = np.cross(a, b)
    w = 1.0 + d
    q = np.concatenate([axis, w], -1)
    # antiparallel: rotate π about any perpendicular of a
    bad = (w[..., 0] < 1e-6)
    if np.any(bad):
        ab = a[bad]
        perp = np.cross(ab, np.broadcast_to([1.0, 0, 0], ab.shape))
        n = np.linalg.norm(perp, axis=-1, keepdims=True)
        alt = np.cross(ab, np.broadcast_to([0, 1.0, 0], ab.shape))
        perp = np.where(n > 1e-6, perp, alt)
        perp /= (np.linalg.norm(perp, axis=-1, keepdims=True) + 1e-12)
        q[bad] = np.concatenate([perp, np.zeros_like(perp[..., :1])], -1)
    return qnorm(q)


def axis_angle(axis, ang):
    h = ang[..., None] * 0.5
    return np.concatenate([axis * np.sin(h), np.cos(h)], -1)


def canonicalize(wq, rest_world, rest_dir):
    """wq[T,J,4] world quats + reference triple → (Q'[T,J,4], valid[J]).

    Q' is the rig-independent training quat; invalid joints (zero restDir)
    are identity with valid=0.
    """
    T = wq.shape[0]
    out = np.zeros((T, J, 4), np.float32)
    out[..., 3] = 1.0
    valid = np.zeros(J, np.float32)
    for c in range(J):
        dr = np.asarray(rest_dir[c], np.float32)
        n = np.linalg.norm(dr)
        if n < 1e-6:
            continue
        dr = dr / n
        qr = qnorm(np.asarray(rest_world[c], np.float32))
        a_s = qrot(qconj(qr)[None], dr[None])[0]         # bone's local axis
        W = qnorm(wq[:, c])                              # [T,4]
        d = qrot(W, np.broadcast_to(a_s, (T, 3)))        # [T,3] world dir
        d = d / (np.linalg.norm(d, axis=-1, keepdims=True) + 1e-12)
        delta = qmul(W, np.broadcast_to(qconj(qr), (T, 4)))
        aim_s = arc(np.broadcast_to(dr, (T, 3)), d)
        tw = qmul(qconj(aim_s), delta)                   # twist about dr
        th = 2.0 * np.arctan2((tw[:, :3] * dr).sum(-1), tw[:, 3])
        th = np.unwrap(th)
        # cap runaway unwrap drift (numeric noise on near-degenerate aims)
        th = np.clip(th, -2.5 * np.pi, 2.5 * np.pi)
        aim_c = arc(np.broadcast_to(D_CANON[c], (T, 3)), d)
        out[:, c] = qmul(axis_angle(d, th), aim_c)
        valid[c] = 1.0
    return qnorm(out), valid


# ---------- corpus source ----------
def corpus_clips(corpus, min_roles):
    """Yield (action, Q'[T,J,4], valid[J], src) from every sidecar dump."""
    manifest = {}
    mpath = os.path.join(corpus, "manifest.json")
    if os.path.exists(mpath):
        manifest = json.load(open(mpath))
    dumps = sorted(glob.glob(os.path.join(corpus, "raw", "**",
                                          "*.canonical.json"), recursive=True))
    for dpath in dumps:
        try:
            dump = json.load(open(dpath))
        except Exception:
            continue
        asset = os.path.basename(os.path.dirname(dpath))
        prov = libv5.manifest_lookup(manifest, asset) or {}
        tags = prov.get("tags", [])
        title = prov.get("title", asset)
        for c in dump.get("clips", []):
            if c.get("resolvedRoles", 0) < min_roles:
                continue
            q = c.get("quats", [])
            rw = c.get("restWorld", [])
            rd = c.get("restDir", [])
            if len(q) < 12 or len(rw) != J or len(rd) != J:
                continue
            action = libv5.action_for(c.get("animation", ""), tags)
            if not action:
                continue
            # the library's uprightness/energy gate (#855) — same hygiene bar
            e = libv5.frame_energy(q)
            me = sum(e) / max(1, len(e))
            if me < 0.004:
                continue
            quality, drop = libv5.clip_quality(
                action, q, rw, rd, c.get("resolvedRoles", 0), me, 0.004)
            if drop:
                continue
            wq = np.asarray(q, np.float32)               # [T,J,4]
            cq, valid = canonicalize(wq, rw, rd)
            yield action, cq, valid, f"{title} — {c.get('animation')}"


# ---------- CMU source ----------
def bvh_rest(path):
    """Rest joint positions from a BVH's OFFSET tree → restDir per canonical
    joint (identity rest rotations ⇒ world dir = offset-chain direction)."""
    from bvh import Bvh
    with open(path) as f:
        m = Bvh(f.read())
    cmap = prep4.resolve_canon(m.get_joints_names())
    if cmap is None:
        return None
    pos = {}
    for name in m.get_joints_names():
        p, cur = np.zeros(3), name
        while cur is not None:
            p = p + np.asarray(m.joint_offset(cur), np.float64)
            try:
                par = m.joint_parent(cur)
                cur = par.name if par else None
            except Exception:
                cur = None
        pos[name] = p
    rd = np.zeros((J, 3), np.float32)
    for c in range(J):
        if c in CHILD:
            v = pos[cmap[CANON[CHILD[c]]]] - pos[cmap[CANON[c]]]
        else:  # tips: head +Y, hands follow the forearm, feet +Z
            if c == 5:
                v = np.array([0, 1.0, 0])
            elif c in (9, 13):
                v = pos[cmap[CANON[c]]] - pos[cmap[CANON[PARENT[c]]]]
            else:
                v = np.array([0, 0, 1.0])
        n = np.linalg.norm(v)
        rd[c] = (v / n) if n > 1e-6 else 0.0
    return rd


def cmu_clips(bvh_dir, index):
    labels = prep4.load_index(index)
    files = sorted(glob.glob(os.path.join(bvh_dir, "**/*.bvh"), recursive=True))
    ident = np.zeros((J, 4), np.float32)
    ident[:, 3] = 1.0
    for path in files:
        mid_m = re.match(r"(\d+_\d+)", os.path.basename(path))
        mid = mid_m.group(1) if mid_m else None
        if mid is None or mid not in labels:
            continue
        r = prep4.parse_bvh(path)
        if r is None:
            continue
        _lq, wq = r
        rd = bvh_rest(path)
        if rd is None:
            continue
        cq, valid = canonicalize(wq, ident, rd)
        yield labels[mid], cq, valid, f"CMU {mid}"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--corpus", default="")
    ap.add_argument("--bvh", default="")
    ap.add_argument("--index", default="")
    ap.add_argument("--out", default="/tmp/t2m_v5.npz")
    ap.add_argument("--T", type=int, default=40)
    ap.add_argument("--min-roles", type=int, default=12)
    ap.add_argument("--min-action-windows", type=int, default=8)
    a = ap.parse_args()

    mo, msk, acts, srcs = [], [], [], []

    def angdist(x, y):
        d = np.abs((x * y).sum(-1)).clip(0, 1)
        return 2.0 * np.arccos(d)

    LOCOMOTION = {"walk", "run", "march"}

    def posture_ok(action, cq, valid):
        """Posture gates (#837 quality follow-up): the v5 model learned a
        head-down hunched walk because unfiltered windows include folded /
        idle-contaminated / placeholder-armed source content. In the
        CANONICAL rep the checks are trivial — d(f) = Q'(f)·D_c:
        spine must stay up, neck/head must stay up, and locomotion arms
        must HANG (signed Y, the library-curation lesson: abs() passes a
        skyward arm)."""
        def mean_y(r):
            d = qrot(cq[:, r], np.broadcast_to(D_CANON[r], (len(cq), 3)))
            return float(d[:, 1].mean())
        floor = 0.7 if action in LOCOMOTION else 0.5
        for r in (0, 1, 2):                    # spine chain
            if valid[r]:
                if mean_y(r) < floor:
                    return False
                break
        for r in (3, 4, 5):                    # neck / head
            if valid[r]:
                if mean_y(r) < 0.5:
                    return False
                break
        if action in LOCOMOTION:
            for r in (7, 11):                  # upper arms hang
                if valid[r] and mean_y(r) > -0.25:
                    return False
        return True

    dropped = [0]

    def window(action, cq, valid, src):
        # The v4 neutral-start gate is deliberately GONE: model clips now ride
        # the bind-referenced direction retarget, which references the
        # reference TRIPLE, not clip frame 0 — any phase is a valid window.
        T, nF = a.T, cq.shape[0]
        if nF < max(16, T // 2):
            return
        if nF < T:
            # pad: loop-tile cyclic clips (end pose ≈ start pose), else hold
            cyc = angdist(cq[-1], cq[0]).mean() < 0.25
            reps = [cq]
            while sum(r.shape[0] for r in reps) < T:
                reps.append(cq if cyc else cq[-1:].repeat(T, 0))
            cq = np.concatenate(reps, 0)[:T]
            nF = T
        for s in range(0, nF - T + 1, max(1, T // 2)):
            w = cq[s:s + T]
            if not posture_ok(action, w, valid):
                dropped[0] += 1
                continue
            mo.append(w)
            msk.append(valid)
            acts.append(action)
            srcs.append(src)

    # Per-item guard: a single malformed clip/BVH must not abort a multi-
    # thousand-item batch (offline dev tool — resilience over strictness).
    if a.corpus:
        n0, bad = 0, 0
        for action, cq, valid, src in corpus_clips(
                os.path.expanduser(a.corpus), a.min_roles):
            try:
                window(action, cq, valid, src); n0 += 1
            except Exception as e:                          # noqa: BLE001
                bad += 1
                print(f"  skip corpus clip ({src}): {e}")
        print(f"corpus: {n0} clips → {len(mo)} windows ({bad} skipped)")
    if a.bvh and a.index:
        n0, w0, bad = 0, len(mo), 0
        for action, cq, valid, src in cmu_clips(a.bvh, a.index):
            try:
                window(action, cq, valid, src); n0 += 1
            except Exception as e:                          # noqa: BLE001
                bad += 1
                print(f"  skip cmu trial ({src}): {e}")
        print(f"cmu: {n0} trials → {len(mo) - w0} windows ({bad} skipped)")

    print(f"posture gates dropped {dropped[0]} windows")
    if not mo:
        sys.exit("no windows extracted")

    # vocab = actions with enough support
    from collections import Counter
    cnt = Counter(acts)
    vocab = sorted(w for w, n in cnt.items() if n >= a.min_action_windows)
    keep = [i for i, w in enumerate(acts) if w in vocab]
    if not vocab or not keep:
        sys.exit(f"no action reached --min-action-windows "
                 f"({a.min_action_windows}); per-action counts: {dict(cnt)}")
    mo = np.stack([mo[i] for i in keep]).astype(np.float32)
    msk = np.stack([msk[i] for i in keep]).astype(np.float32)
    tk = np.zeros((len(keep), len(vocab)), np.float32)
    for r, i in enumerate(keep):
        tk[r, vocab.index(acts[i])] = 1.0
    print(f"windows: {mo.shape[0]}  vocab({len(vocab)}):",
          {w: int(tk[:, vocab.index(w)].sum()) for w in vocab})
    np.savez_compressed(a.out, mo=mo, msk=msk, tk=tk,
                        vocab=np.array(vocab), fps=FPS,
                        canonRestDir=D_CANON)
    print(f"wrote {a.out}  mo{mo.shape}")


if __name__ == "__main__":
    main()
