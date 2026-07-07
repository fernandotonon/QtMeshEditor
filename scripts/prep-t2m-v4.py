#!/usr/bin/env python3
# ruff: noqa: E702, E741
"""Preprocess CMU BVH into the v4 text-to-motion training cache (#411).

ONE-TIME OFFLINE dev tool — NOT shipped. Replaces prep-t2m-clean.py with the
fixes that the v3 model's failure diagnosis demanded:

  1. 30 FPS windows (was RAW 120fps). The runtime plays model clips at 30fps
     (MotionGenerator clip.fps=30, same as the template library), and a
     40-frame 120fps window is 0.33 s — a third of a walk cycle. v4 windows
     are T frames at 30 fps (T=60 → 2 s), matching playback exactly.
  2. WORLD-frame FK quats (was local Euler-composed). The v3 library retarget
     (`AnimationMerger::applyMotionClip` worldFrame=true) is the good path —
     basis-independent, no per-bone roll ambiguity. Training in the SAME
     representation lets model clips use it too (vocab json: "frame":"world").
  3. NEUTRAL-START windows. The retarget composes every frame as a delta
     against clip frame 0, so a window must OPEN on a calm, standing-like
     pose. Windows start only at low-velocity, near-T-pose frames.
  4. Labels from the PUBLIC CMU index (cmu-mocap-index-text.txt, in the BVH
     conversion repo) — single-action trials only, plus curated per-subject
     includes for the actions whose descriptions don't keyword-match (salsa
     → dance, "direct traffic" → wave, subject 09 → run).

Output: mo[N,T,22,4] float32 WORLD quats + tk[N,V] one-hot, ready for
train-t2m-onnx-v4.py.

Usage:
  python prep-t2m-v4.py --bvh /tmp/cmu-mocap-bvh/data \
      --index /tmp/cmu_index.txt --out /tmp/t2m_v4.npz [--T 60]
"""
import argparse
import glob
import os
import re

import numpy as np

CANON = ["hip", "abdomen", "chest", "neck", "neck1", "head",
         "rcollar", "rshoulder", "relbow", "rhand",
         "lcollar", "lshoulder", "lelbow", "lhand",
         "rbuttock", "rhip", "rknee", "rfoot",
         "lbuttock", "lhip", "lknee", "lfoot"]
DROP = ("jaw", "oris", "tongue", "levator", "special", "eye", "orbicularis",
        "temporalis", "oculi", "risorius", "finger", "metacarpal", "toe",
        "thumb", "__")
J = len(CANON)
OUT_FPS = 30

# Canonical joint → accepted source-joint names, per BVH conversion flavour:
# the Daz-friendly release uses lowercase anatomical names (hip, abdomen,
# rcollar…), the MotionBuilder-friendly release (una-dinosauria/cmu-mocap)
# uses Hips/LowerBack/RightShoulder…. FK world composition walks the FULL
# joint tree, so extra intermediate joints (Spine between LowerBack and
# Spine1) stay accounted for either way.
ALIASES = {
    "hip":       ["hip", "Hips"],
    "abdomen":   ["abdomen", "LowerBack"],
    "chest":     ["chest", "Spine1"],
    "neck":      ["neck", "Neck"],
    "neck1":     ["neck1", "Neck1"],
    "head":      ["head", "Head"],
    "rcollar":   ["rcollar", "RightShoulder"],
    "rshoulder": ["rshoulder", "RightArm"],
    "relbow":    ["relbow", "RightForeArm"],
    "rhand":     ["rhand", "RightHand"],
    "lcollar":   ["lcollar", "LeftShoulder"],
    "lshoulder": ["lshoulder", "LeftArm"],
    "lelbow":    ["lelbow", "LeftForeArm"],
    "lhand":     ["lhand", "LeftHand"],
    "rbuttock":  ["rbuttock", "RHipJoint"],
    "rhip":      ["rhip", "RightUpLeg"],
    "rknee":     ["rknee", "RightLeg"],
    "rfoot":     ["rfoot", "RightFoot"],
    "lbuttock":  ["lbuttock", "LHipJoint"],
    "lhip":      ["lhip", "LeftUpLeg"],
    "lknee":     ["lknee", "LeftLeg"],
    "lfoot":     ["lfoot", "LeftFoot"],
}


def resolve_canon(names):
    """Map each canonical joint to its source-joint name, or None if any is
    missing (non-humanoid / unexpected skeleton)."""
    out = {}
    nameset = set(names)
    for c in CANON:
        hit = next((a for a in ALIASES[c] if a in nameset), None)
        if hit is None:
            return None
        out[c] = hit
    return out


# v4 vocab — mirrors the template library's action set so both motion sources
# cover the same prompts.
VOCAB = ["walk", "run", "jump", "dance", "march", "kick", "punch", "wave",
         "climb", "sit", "throw", "boxing", "idle"]

# description-keyword → action (single hit required), applied to the index.
KEYWORDS = {
    "walk": "walk", "run": "run", "jog": "run", "jump": "jump",
    "dance": "dance", "salsa": "dance", "lambada": "dance",
    "march": "march", "kick": "kick", "punch": "punch", "boxing": "boxing",
    "wave": "wave", "climb": "climb", "sit": "sit", "throw": "throw",
    "wait": "idle",
}
# curated subject/trial overrides where descriptions keyword-match poorly.
OVERRIDES = {
    re.compile(r"^09_"): "run",                    # subject 09 = run
    re.compile(r"^60_"): "dance",                  # salsa
    re.compile(r"^13_2[678]$|^14_2[456]$"): "wave",  # direct traffic, wave
    re.compile(r"^40_1[01]$"): "idle",             # wait for bus
    re.compile(r"^111_33$"): "throw",
}


def is_core(n): return not any(k in n.lower() for k in DROP)


def euler_to_quat_vec(rad, order):
    Tn = rad.shape[0]; axis_of = {"X": 0, "Y": 1, "Z": 2}
    def axisq(angle, axis):
        h = angle * 0.5; q = np.zeros((Tn, 4), np.float32)
        q[:, 3] = np.cos(h); q[:, axis] = np.sin(h); return q
    def qmul(a, b):
        ax, ay, az, aw = a[:, 0], a[:, 1], a[:, 2], a[:, 3]
        bx, by, bz, bw = b[:, 0], b[:, 1], b[:, 2], b[:, 3]
        o = np.empty_like(a)
        o[:, 0] = aw*bx + ax*bw + ay*bz - az*by
        o[:, 1] = aw*by - ax*bz + ay*bw + az*bx
        o[:, 2] = aw*bz + ax*by - ay*bx + az*bw
        o[:, 3] = aw*bw - ax*bx - ay*by - az*bz
        return o
    q = np.zeros((Tn, 4), np.float32); q[:, 3] = 1.0
    for ci, ch in enumerate(order):
        q = qmul(q, axisq(rad[:, ci], axis_of[ch]))
    return q


def parse_bvh(path):
    """One BVH -> (local[T,J,4], world[T,J,4]) canonical quats @ OUT_FPS, or None."""
    from bvh import Bvh
    with open(path) as f:
        m = Bvh(f.read())
    cmap = resolve_canon(m.get_joints_names())
    if cmap is None:
        return None
    all_names = m.get_joints_names()
    frames = np.array(m.frames, dtype=np.float32)
    nF = len(frames)
    if nF < 8:
        return None
    col, rotcols, roto, parent = 0, {}, {}, {}
    for j in all_names:
        chans = m.joint_channels(j); rc, order = [], ""
        for ci, c in enumerate(chans):
            if c.endswith("rotation"):
                rc.append(col + ci); order += c[0]
        rotcols[j], roto[j] = rc, order
        try:
            p = m.joint_parent(j); parent[j] = p.name if p else None
        except Exception:
            parent[j] = None
        col += len(chans)
    local = {}
    for j in all_names:
        if len(roto[j]) == 3:
            local[j] = euler_to_quat_vec(np.deg2rad(frames[:, rotcols[j]]), roto[j])
        else:
            q = np.zeros((nF, 4), np.float32); q[:, 3] = 1.0; local[j] = q
    # FK world = parentWorld * local (vectorised per joint)
    def qmul_arr(a, b):
        ax, ay, az, aw = a[:, 0], a[:, 1], a[:, 2], a[:, 3]
        bx, by, bz, bw = b[:, 0], b[:, 1], b[:, 2], b[:, 3]
        o = np.empty_like(a)
        o[:, 0] = aw*bx + ax*bw + ay*bz - az*by
        o[:, 1] = aw*by - ax*bz + ay*bw + az*bx
        o[:, 2] = aw*bz + ax*by - ay*bx + az*bw
        o[:, 3] = aw*bw - ax*bx - ay*by - az*bz
        return o
    world = {}
    def world_of(j):
        if j in world: return world[j]
        w = local[j]
        p = parent.get(j)
        if p is not None and p in local:
            w = qmul_arr(world_of(p), w)
        world[j] = w
        return w
    lq = np.stack([local[cmap[j]] for j in CANON], 1)     # [T,J,4]
    wq = np.stack([world_of(cmap[j]) for j in CANON], 1)  # [T,J,4]
    try:
        src_fps = round(1.0 / float(m.frame_time))
    except Exception:
        src_fps = 120
    stride = max(1, round(src_fps / OUT_FPS))
    lq, wq = lq[::stride], wq[::stride]
    lq /= (np.linalg.norm(lq, axis=-1, keepdims=True) + 1e-8)
    wq /= (np.linalg.norm(wq, axis=-1, keepdims=True) + 1e-8)
    return lq, wq


def _angdist(a, b):
    d = np.abs((a * b).sum(-1)).clip(0.0, 1.0)
    return 2.0 * np.arccos(d)


def neutral_starts(lq, T, idle=False, skip=15):
    """Start indices for T-frame windows that OPEN calm+near-neutral and (for
    non-idle actions) MOVE inside. Returns a list of ints."""
    nF = lq.shape[0]
    skip = min(skip, max(0, nF - T))     # short trials: keep at least 1 window
    if nF < T:
        return []
    vel = _angdist(lq[1:], lq[:-1]).mean(-1)               # [T-1] rad/frame
    vel = np.convolve(vel, np.ones(5) / 5.0, mode="same")
    neut = _angdist(lq, lq[:1]).mean(-1)                   # vs trial T-pose
    calm_v = np.quantile(vel, 0.35)
    calm_n = np.quantile(neut, 0.5)
    starts = []
    s = skip
    while s <= nF - T:
        if vel[min(s, len(vel) - 1)] <= calm_v and neut[s] <= calm_n:
            inner = vel[s:s + T - 1].mean()
            ok = (inner < calm_v) if idle else (inner > np.quantile(vel, 0.45))
            if ok:
                starts.append(s)
                s += T // 3                                # hop a third-window
                continue
        s += 3
    # a trial with NO qualifying start still yields its calmest start — rare
    # actions (punch/throw) live in short trials that the gates over-prune
    if not starts and nF - T >= 0:
        cand = np.arange(skip, nF - T + 1)
        if len(cand):
            starts.append(int(cand[int(np.argmin(vel[cand]))]))
    return starts


def load_index(path):
    """index txt -> {motion_id: action} using single-keyword hits + overrides."""
    labels = {}
    for line in open(path, errors="ignore"):
        m = re.match(r"^(\d+_\d+)\s+(.*)$", line.strip())
        if not m:
            continue
        mid, desc = m.group(1), m.group(2).lower()
        if "2 subjects" in desc:
            continue                                   # interaction trials
        forced = None
        for rx, act in OVERRIDES.items():
            if rx.match(mid):
                forced = act; break
        if forced:
            labels[mid] = forced; continue
        hits = {KEYWORDS[k] for k in KEYWORDS if re.search(r"\b" + k, desc)}
        if len(hits) == 1:
            labels[mid] = hits.pop()
    return labels


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bvh", required=True)
    ap.add_argument("--index", required=True)
    ap.add_argument("--out", default="/tmp/t2m_v4.npz")
    ap.add_argument("--T", type=int, default=60, help="window length @30fps")
    a = ap.parse_args()

    labels = load_index(a.index)
    from collections import Counter
    print(f"labelled trials: {len(labels)}  per-action:",
          dict(Counter(labels.values())))

    files = sorted(glob.glob(os.path.join(a.bvh, "**/*.bvh"), recursive=True))
    print(f"found {len(files)} BVH files")
    mo, tk, srcs = [], [], []
    parsed = skipped = 0
    for path in files:
        mid_m = re.match(r"(\d+_\d+)", os.path.basename(path))
        mid = mid_m.group(1) if mid_m else None
        if mid is None or mid not in labels:
            continue
        r = parse_bvh(path)
        if r is None:
            skipped += 1; continue
        lq, wq = r
        act = labels[mid]
        for s in neutral_starts(lq, a.T, idle=(act == "idle")):
            mo.append(wq[s:s + a.T])
            v = np.zeros(len(VOCAB), np.float32); v[VOCAB.index(act)] = 1.0
            tk.append(v); srcs.append(mid)
        parsed += 1
    print(f"parsed {parsed} trials (skipped {skipped} non-canonical), "
          f"windows: {len(mo)}")
    if not mo:
        raise SystemExit("no windows — check --bvh / --index")
    mo = np.stack(mo); tk = np.stack(tk)
    cnt = tk.sum(0).astype(int)
    print("windows per action:",
          " ".join(f"{VOCAB[i]}={cnt[i]}" for i in range(len(VOCAB))))
    np.savez(a.out, mo=mo, tk=tk, vocab=np.array(VOCAB), fps=OUT_FPS)
    print(f"wrote {a.out}  mo{mo.shape} tk{tk.shape}")


if __name__ == "__main__":
    main()
