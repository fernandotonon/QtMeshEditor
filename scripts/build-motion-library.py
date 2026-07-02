#!/usr/bin/env python3
# ruff: noqa: E702, E741
"""Build the text-to-motion template clip library (#411 MVP).

ONE-TIME, OFFLINE developer tool — NOT shipped; the app never runs Python.

The #411 spike showed a from-scratch GENERATIVE text-to-motion model collapses
without multi-day ML effort (see docs/TEXT_TO_MOTION_SPIKE_411.md). The shipped
MVP is instead a TEMPLATE-CLIP approach: a small curated library of permissive
motion clips, matched to a prompt by action keyword, then retargeted onto the
user's skeleton via the #409 canonical-joint mapping.

This tool curates representative clips from the CMU MoCap database
(mocap.cs.cmu.edu — permissively licensed, commercial-OK, same source as the
#409 RMIB model) into ONE compact JSON the app downloads on first `--generate`
use (hosted on the HF models repo, like the ONNX models).

OUTPUT: motion-library.json
  {
    "schema": "qtmesh-motion-library-v1",
    "joints": [22 canonical joint names],   # MUST match MotionInbetween kCanonJoints
    "fps": 30,
    "clips": [
      { "action": "walk", "source": "CMU 02_01", "frames": N,
        "quats": [[ [x,y,z,w] * 22 ] * N] }   # per-frame, per-joint unit quats
    ]
  }

Usage (offline, with numpy + bvh in a venv):
    python build-motion-library.py --bvh <cmu_bvh_dir> \
        --annotations <csv> --out motion-library.json
"""
import argparse
import csv
import glob
import json
import os
import re

import numpy as np

# Canonical 22-joint skeleton — MUST match src/MotionInbetween.cpp kCanonJoints.
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

MAX_FRAMES = 120          # cap clip length (~4s @ 30fps); keeps the library small

# One clean, representative CMU clip per action (motion-id, action label).
# Picked from the cgspeed/CMU index (cmu-mocap-index-text.txt) for short
# single-action descriptions, then verified by rendering each retargeted clip.
# v4 fixes: 69_01 was actually "walk forward" (not idle) → 40_10 "wait for
# bus"; 05_02 ballet (pirouettes fold badly with a locked root) → 60_08 salsa;
# plus sit / throw / boxing coverage.
CURATED = [
    # Several takes per action: matchPrompt picks among same-action clips at
    # random, so repeat generates give VARIETY with real-mocap quality — the
    # practical answer to "generative" until a licensable model exists.
    ("07_01", "walk"), ("07_02", "walk"), ("08_01", "walk"),
    ("08_06", "walk"), ("02_01", "walk"), ("38_01", "walk"),
    ("09_01", "run"), ("09_02", "run"), ("09_05", "run"), ("02_03", "run"),
    ("16_01", "jump"), ("16_05", "jump"), ("13_11", "jump"), ("13_19", "jump"),
    ("60_02", "dance"), ("60_05", "dance"), ("60_08", "dance"),
    ("60_12", "dance"), ("55_02", "dance"),
    ("20_06", "march"), ("21_06", "march"),
    ("10_01", "kick"), ("10_02", "kick"), ("10_05", "kick"), ("11_01", "kick"),
    ("02_05", "punch"),
    ("13_26", "wave"), ("13_27", "wave"), ("14_24", "wave"),
    ("01_02", "climb"), ("01_04", "climb"),
    ("40_10", "idle"), ("40_11", "idle"),
    ("13_01", "sit"), ("13_02", "sit"), ("13_03", "sit"),
    ("111_33", "throw"),
    ("13_17", "boxing"), ("13_18", "boxing"), ("14_01", "boxing"),
    ("14_02", "boxing"), ("15_13", "boxing"), ("17_10", "boxing"),
    ("13_23", "sweep"), ("13_24", "sweep"),
    ("13_20", "wash"), ("13_21", "wash"),
]

# ---- active-window selection -------------------------------------------------
# CMU trials often start with idle standing / walking into position, so the
# FIRST seconds rarely contain the labelled action (the old builder's clips of
# "wave" never waved). Pick the L-frame window with the highest motion energy
# (lowest for idle), and SNAP the window start to a nearby low-energy,
# NEAR-NEUTRAL frame: the retarget (AnimationMerger::applyMotionClip) composes
# every frame as a delta against clip frame 0, so the window must begin at a
# calm, standing-like pose for the deltas to be true articulations.

def _angdist(a, b):
    """Per-element angular distance between two unit-quat arrays [..., 4]."""
    d = np.abs((a * b).sum(-1)).clip(0.0, 1.0)
    return 2.0 * np.arccos(d)


def select_window(local_q, L, idle=False, skip=30):
    """Return the best start index for an L-frame window over [T,J,4] LOCAL
    quats (30fps). Energy = mean joint angular velocity; neutrality = pose
    distance to the trial's frame 0 (the conversion's T-pose)."""
    nF = local_q.shape[0]
    if nF <= L:
        return 0
    vel = _angdist(local_q[1:], local_q[:-1]).mean(-1)          # [T-1]
    vel = np.convolve(vel, np.ones(5) / 5.0, mode="same")        # smooth
    neut = _angdist(local_q, local_q[:1]).mean(-1)               # [T] vs T-pose
    lo = min(skip, nF - L)
    starts = np.arange(lo, nF - L + 1)
    act = np.array([vel[s:s + L - 1].mean() for s in starts])
    if idle:
        score = -(act - act.mean()) / (act.std() + 1e-9)                 - (neut[starts] - neut[starts].mean()) / (neut[starts].std() + 1e-9)
    else:
        score = (act - act.mean()) / (act.std() + 1e-9)                 - 0.75 * (neut[starts] - neut[starts].mean()) / (neut[starts].std() + 1e-9)
    s = int(starts[int(np.argmax(score))])
    # snap the start to the calmest frame in a ±0.5 s neighbourhood so the
    # clip opens on a settled pose (the delta reference)
    a = max(lo, s - 15); b = min(nF - L, s + 15)
    if b > a:
        s = int(a + np.argmin(vel[a:b]))
    return s


def is_core(name):
    return not any(k in name.lower() for k in DROP)


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


def clip_quats(path):
    """Parse one BVH -> [T, J, 4] canonical unit quats, resampled to OUT_FPS."""
    from bvh import Bvh
    with open(path) as f:
        m = Bvh(f.read())
    cmap = resolve_canon(m.get_joints_names())
    if cmap is None:
        return None
    frames = np.array(m.frames, dtype=np.float32)
    nF = len(frames)
    if nF < 4:
        return None
    col, rotcols, roto = 0, {}, {}
    for j in m.get_joints_names():
        chans = m.joint_channels(j); rc, order = [], ""
        for ci, c in enumerate(chans):
            if c.endswith("rotation"):
                rc.append(col + ci); order += c[0]
        if len(order) == 3:
            rotcols[j], roto[j] = rc, order
        col += len(chans)
    quats = np.zeros((nF, J, 4), np.float32)
    for ji, j in enumerate(CANON):
        src = cmap[j]
        if src not in rotcols:
            quats[:, ji, 3] = 1.0; continue
        quats[:, ji] = euler_to_quat_vec(np.deg2rad(frames[:, rotcols[src]]), roto[src])
    # CMU is 120fps; resample to OUT_FPS by simple stride, cap length.
    try:
        src_fps = round(1.0 / float(m.frame_time))
    except Exception:
        src_fps = 120
    stride = max(1, round(src_fps / OUT_FPS))
    quats = quats[::stride]                 # full trial; windowed in main()
    # renormalise
    quats /= (np.linalg.norm(quats, axis=-1, keepdims=True) + 1e-8)
    return quats


def _qmul1(a, b):
    """Single-quat (x,y,z,w) Hamilton product."""
    ax, ay, az, aw = a; bx, by, bz, bw = b
    return np.array([aw*bx + ax*bw + ay*bz - az*by,
                     aw*by - ax*bz + ay*bw + az*bx,
                     aw*bz + ax*by - ay*bx + az*bw,
                     aw*bw - ax*bx - ay*by - az*bz], np.float32)


def clip_world_quats(path):
    """Parse one BVH -> [T, J, 4] canonical WORLD-space unit quats (schema v3).

    Runs forward kinematics over the FULL BVH joint tree (rotations only — the
    rest offsets carry no orientation in BVH, so world rotation = product of
    local rotations down the chain) and projects the result to the 22 canonical
    joints. World-space orientations are basis-independent, so the retarget can
    take a clean world delta with NO local-frame roll ambiguity (the per-bone
    arm-twist the local-quat approach left behind). Resampled to OUT_FPS.
    """
    from bvh import Bvh
    with open(path) as f:
        m = Bvh(f.read())
    cmap = resolve_canon(m.get_joints_names())
    if cmap is None:
        return None
    all_names = m.get_joints_names()
    frames = np.array(m.frames, dtype=np.float32)
    nF = len(frames)
    if nF < 4:
        return None
    # column layout + parent map over ALL joints (FK needs the full chain)
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
    # local quats for every joint
    local = {}
    for j in all_names:
        if len(roto[j]) == 3:
            local[j] = euler_to_quat_vec(np.deg2rad(frames[:, rotcols[j]]), roto[j])
        else:
            q = np.zeros((nF, 4), np.float32); q[:, 3] = 1.0; local[j] = q
    # FK: world[j] = world[parent] * local[j], memoised over the tree
    world = {}
    def world_of(j):
        if j in world: return world[j]
        w = local[j].copy()
        p = parent.get(j)
        if p is not None and p in local:
            wp = world_of(p)
            # per-frame quat product wp * w
            out = np.empty_like(w)
            for f in range(nF):
                out[f] = _qmul1(wp[f], w[f])
            w = out
        world[j] = w
        return w
    quats = np.zeros((nF, J, 4), np.float32)
    for ji, j in enumerate(CANON):
        quats[:, ji] = world_of(cmap[j])
    # CMU is 120fps; resample to OUT_FPS by simple stride, cap length.
    try:
        src_fps = round(1.0 / float(m.frame_time))
    except Exception:
        src_fps = 120
    stride = max(1, round(src_fps / OUT_FPS))
    quats = quats[::stride]                 # full trial; windowed in main()
    quats /= (np.linalg.norm(quats, axis=-1, keepdims=True) + 1e-8)
    return quats


def find_bvh(bvh_dir, mid):
    hits = glob.glob(os.path.join(bvh_dir, "**", mid + ".bvh"), recursive=True)
    return hits[0] if hits else None


def _quat_from_to(a, b):
    """Shortest-arc quaternion (x,y,z,w) rotating unit vector a onto unit b."""
    a = a / (np.linalg.norm(a) + 1e-9); b = b / (np.linalg.norm(b) + 1e-9)
    d = float(np.dot(a, b))
    if d > 0.999999:
        return np.array([0, 0, 0, 1], np.float32)
    if d < -0.999999:
        # 180°: pick any perpendicular axis
        axis = np.cross(a, [1, 0, 0])
        if np.linalg.norm(axis) < 1e-6: axis = np.cross(a, [0, 1, 0])
        axis /= np.linalg.norm(axis)
        return np.array([axis[0], axis[1], axis[2], 0], np.float32)
    axis = np.cross(a, b); s = np.sqrt((1 + d) * 2);
    q = np.array([axis[0] / s, axis[1] / s, axis[2] / s, s / 2], np.float32)
    return q / (np.linalg.norm(q) + 1e-9)


def cmu_rest_world(path):
    """Per-canonical-joint WORLD-REST orientation (x,y,z,w) for the CMU skeleton.

    The CMU rest is a T-pose: each bone points along its child's OFFSET. We model
    each joint's rest orientation as the shortest-arc rotation that takes a
    reference axis (+Y, the BVH 'bone-down-its-length' convention) onto the bone's
    actual rest DIRECTION (offset to its first canonical child). This captures
    direction; per-bone roll about the bone axis is left at the shortest-arc
    default (CMU bones have no extra authored roll — they're axis-aligned), which
    is what the retarget change-of-basis needs to cancel the CMU↔target twist.
    """
    from bvh import Bvh
    m = Bvh(open(path).read())
    names = m.get_joints_names()
    # child map within the canonical set: first canonical child per joint
    parent_of = {}
    for j in names:
        for k in names:
            try:
                if j in [c.name for c in m.joint_direct_children(k)]:
                    parent_of[j] = k
            except Exception:
                pass
    # direction of a joint's bone = offset of its first canonical child
    def bone_dir(j):
        best = None
        for k in CANON:
            if parent_of.get(k) == j:
                off = np.array(m.joint_offset(k), np.float32)
                if np.linalg.norm(off) > 1e-3:
                    best = off; break
        if best is None:  # leaf: use own offset direction
            off = np.array(m.joint_offset(j), np.float32)
            best = off if np.linalg.norm(off) > 1e-3 else np.array([0, 1, 0], np.float32)
        return best
    rest = []
    for j in CANON:
        d = bone_dir(j)
        q = _quat_from_to(np.array([0, 1, 0], np.float32), d)   # +Y → bone dir
        rest.append([round(float(v), 5) for v in q])
    return rest


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bvh", required=True)
    ap.add_argument("--annotations", default=None)
    ap.add_argument("--out", default="motion-library.json")
    a = ap.parse_args()

    clips = []
    for mid, action in CURATED:
        path = find_bvh(a.bvh, mid)
        if not path:
            print(f"  skip {action} ({mid}): BVH not found"); continue
        # WORLD-space joint orientations (schema v3) — basis-independent, so the
        # retarget delta carries the true per-bone roll (no arm-twist).
        wq = clip_world_quats(path)
        lq = clip_quats(path)
        if wq is None or lq is None:
            print(f"  skip {action} ({mid}): not canonical / too short"); continue
        s = select_window(lq, MAX_FRAMES, idle=(action == "idle"))
        q = wq[s:s + MAX_FRAMES]
        print(f"    window [{s}:{s + q.shape[0]}] of {wq.shape[0]} frames")
        clips.append({
            "action": action,
            "source": f"CMU {mid}",
            "frames": int(q.shape[0]),
            "quats": [[[round(float(v), 5) for v in q[t, j]] for j in range(J)]
                      for t in range(q.shape[0])],
        })
        print(f"  added {action} ({mid}): {q.shape[0]} frames @ {OUT_FPS}fps")

    if not clips:
        raise SystemExit("No clips built — check --bvh points at the CMU BVH set.")
    # Schema v3: per-joint quats are WORLD-space (FK product down the BVH tree),
    # marked by "frame":"world". The retarget takes a world delta vs frame 0 and
    # transports it into each target bone's standing frame — no CMU rest needed,
    # no local-frame roll ambiguity. v1/v2 (local quats) still parse for back-compat.
    lib = {"schema": "qtmesh-motion-library-v3", "joints": CANON,
           "fps": OUT_FPS, "frame": "world", "clips": clips}
    with open(a.out, "w") as f:
        json.dump(lib, f)
    sz = os.path.getsize(a.out) / 1024
    print(f"wrote {a.out} — {len(clips)} clips, {sz:.0f} KB")


if __name__ == "__main__":
    main()
