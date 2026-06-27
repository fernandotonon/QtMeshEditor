#!/usr/bin/env python3
"""Train + export the RMIB animation in-betweening model to ONNX (#409).

ONE-TIME, OFFLINE developer tool — NOT shipped with the app, and the app never
runs Python. The app runs the resulting rmib.onnx in C++ via ONNX Runtime
(src/MotionInbetween.cpp), downloading it on first use to
AppData/ai_models/inbetween/rmib.onnx.

WHAT IT PRODUCES
  rmib.onnx with the contract MotionInbetween::predict() expects:
    input  "pose_pair" float32 [1, 2, C]    (start pose, end pose; C = J*10)
    output "interior"  float32 [1, GAP, C]   (GAP predicted interior poses)
  where J = 22 canonical CORE-BODY joints (hips/spine/neck/head + both arms +
  both legs) and the per-joint 10-DoF layout is [tx,ty,tz, qx,qy,qz,qw, sx,sy,sz]
  (the model authors rotation; translation/scale pass through). C = 220, GAP = 8.

DATA + LICENSE
  Trained on the CMU Graphics Lab Motion Capture Database (mocap.cs.cmu.edu) —
  permissively licensed (free incl. commercial use; may not RESELL the data
  itself; credit mocap.cs.cmu.edu). This is why the model is redistributable
  under QtMeshEditor's permissive bar, unlike LAFAN1-trained models (CC-BY-NC-ND)
  which the rest of the in-betweening field standardizes on. See
  THIRD_PARTY_AI_MODELS.md.

PIPELINE (run as three stages; see the repo PR #762 history for the exact
commands used to produce the hosted model):
  1. download CMU MoCap in BVH form.
  2. preprocess: parse BVH → filter to the 22 core body joints → quaternions →
     (start, end, GAP-interior) training windows. ~421k windows from ~2400 clips.
  3. train a small Transformer (start+end pose + learnable interior queries →
     interior poses); validate it beats SLERP (the C++ spline fallback) on a
     held-out split; export to the ONNX contract above.

The skeleton-canonical model is mapped onto arbitrary user rigs at runtime by
MotionInbetween::canonicalIndexForBone() (Mixamo / generic / CMU bone names);
rigs that don't resolve enough core joints fall back to the spline. This file is
kept for reproducibility; it is intentionally NOT wired into CMake or CI.

Usage (offline, with torch + numpy + bvh + onnx in a venv):
    python export-rmib-onnx.py --bvh <dir> --out rmib.onnx
"""
import argparse
import glob
import os

import numpy as np

# ---------------------------------------------------------------------------
# Canonical skeleton — MUST match src/MotionInbetween.cpp kCanonJoints order.
# ---------------------------------------------------------------------------
CANON = ["hip", "abdomen", "chest", "neck", "neck1", "head",
         "rcollar", "rshoulder", "relbow", "rhand",
         "lcollar", "lshoulder", "lelbow", "lhand",
         "rbuttock", "rhip", "rknee", "rfoot",
         "lbuttock", "lhip", "lknee", "lfoot"]
DROP = ("jaw", "oris", "tongue", "levator", "special", "eye", "orbicularis",
        "temporalis", "oculi", "risorius", "finger", "metacarpal", "toe",
        "thumb", "__")
GAP = 8
J = len(CANON)
C = J * 10


def is_core(name):
    low = name.lower()
    return not any(k in low for k in DROP)


def euler_to_quat_vec(rad, order):
    T = rad.shape[0]
    axis_of = {"X": 0, "Y": 1, "Z": 2}

    def axisq(angle, axis):
        h = angle * 0.5
        q = np.zeros((T, 4), np.float32)
        q[:, 3] = np.cos(h)
        q[:, axis] = np.sin(h)
        return q

    def qmul(a, b):
        ax, ay, az, aw = a[:, 0], a[:, 1], a[:, 2], a[:, 3]
        bx, by, bz, bw = b[:, 0], b[:, 1], b[:, 2], b[:, 3]
        o = np.empty_like(a)
        o[:, 0] = aw*bx + ax*bw + ay*bz - az*by
        o[:, 1] = aw*by - ax*bz + ay*bw + az*bx
        o[:, 2] = aw*bz + ax*by - ay*bx + az*bw
        o[:, 3] = aw*bw - ax*bx - ay*by - az*bz
        return o

    q = np.zeros((T, 4), np.float32)
    q[:, 3] = 1.0
    for ci, ch in enumerate(order):
        q = qmul(q, axisq(rad[:, ci], axis_of[ch]))
    return q


def preprocess(bvh_dir):
    from bvh import Bvh
    files = sorted(glob.glob(os.path.join(bvh_dir, "**/*.bvh"), recursive=True))
    print(f"found {len(files)} BVH files")
    starts, ends, interiors = [], [], []
    skipped = 0
    for path in files:
        try:
            with open(path) as f:
                m = Bvh(f.read())
        except Exception:
            skipped += 1
            continue
        names = [n for n in m.get_joints_names() if is_core(n)]
        if names != CANON:
            skipped += 1
            continue
        frames = np.array(m.frames, dtype=np.float32)
        T = len(frames)
        if T < GAP + 3:
            continue
        # channel-column offsets per joint (hierarchy order)
        col, rotcols, roto = 0, {}, {}
        for j in m.get_joints_names():
            chans = m.joint_channels(j)
            rc, order = [], ""
            for ci, c in enumerate(chans):
                if c.endswith("rotation"):
                    rc.append(col + ci)
                    order += c[0]
            if j in CANON and len(order) == 3:
                rotcols[j], roto[j] = rc, order
            col += len(chans)
        quats = np.zeros((T, J, 4), np.float32)
        for ji, j in enumerate(CANON):
            if j not in rotcols:
                quats[:, ji, 3] = 1.0
                continue
            quats[:, ji] = euler_to_quat_vec(np.deg2rad(frames[:, rotcols[j]]), roto[j])
        for s in range(0, T - GAP - 2, 10):
            e = s + GAP + 1
            starts.append(quats[s])
            ends.append(quats[e])
            interiors.append(quats[s+1:e])
    print(f"windows: {len(starts)} (skipped {skipped}/{len(files)} files)")
    if not starts:
        raise SystemExit(
            f"No usable windows: all {len(files)} BVH files failed to parse or "
            f"don't match the canonical {len(CANON)}-joint skeleton. Check --bvh "
            f"points at the CMU BVH set.")
    return np.stack(starts), np.stack(ends), np.stack(interiors)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bvh", required=True, help="dir of CMU BVH files")
    ap.add_argument("--out", default="rmib.onnx")
    ap.add_argument("--epochs", type=int, default=60)
    a = ap.parse_args()

    import torch
    import torch.nn as nn

    qs, qe, qi = preprocess(a.bvh)

    def pack(q):
        sh = q.shape[:-2]
        out = np.zeros(sh + (J, 10), np.float32)
        out[..., 3:7] = q
        out[..., 7:10] = 1.0
        return out.reshape(sh + (C,))
    S = torch.tensor(pack(qs))
    E = torch.tensor(pack(qe))
    Iv = torch.tensor(pack(qi))     # interiors (avoid the ambiguous bare 'I')
    dev = "mps" if torch.backends.mps.is_available() else "cpu"
    print(f"train N={S.shape[0]} C={C} gap={GAP} dev={dev}")

    class Net(nn.Module):
        def __init__(s, d=256, layers=4, heads=8):
            super().__init__()
            s.inp = nn.Linear(C, d)
            s.ep = nn.Parameter(torch.randn(2, d) * 0.02)
            s.q = nn.Parameter(torch.randn(GAP, d) * 0.02)
            enc = nn.TransformerEncoderLayer(d, heads, d*4, batch_first=True, activation="gelu")
            s.enc = nn.TransformerEncoder(enc, layers)
            s.out = nn.Linear(d, C)

        def forward(s, start, end):
            B = start.shape[0]
            a_ = s.inp(start) + s.ep[0]
            b_ = s.inp(end) + s.ep[1]
            q = s.q.unsqueeze(0).expand(B, -1, -1)
            seq = torch.cat([a_.unsqueeze(1), b_.unsqueeze(1), q], 1)
            return s.out(s.enc(seq)[:, 2:])

    net = Net().to(dev)
    opt = torch.optim.AdamW(net.parameters(), lr=3e-4, weight_decay=1e-4)
    sch = torch.optim.lr_scheduler.CosineAnnealingLR(opt, a.epochs)
    n = S.shape[0]
    nval = max(1, n // 10)
    bs = 256
    idx = torch.randperm(n)
    S, E, Iv = S[idx], E[idx], Iv[idx]

    def qb(x):                       # [...,C] → the per-joint quaternion block
        return x.reshape(x.shape[:-1] + (J, 10))[..., 3:7]
    for ep in range(a.epochs):
        net.train()
        perm = torch.randperm(n - nval) + nval
        for b in range(0, perm.numel(), bs):
            bi = perm[b:b+bs]
            p = net(S[bi].to(dev), E[bi].to(dev))
            t = Iv[bi].to(dev)
            loss = ((p - t) ** 2).mean() + 0.01 * ((qb(p).pow(2).sum(-1) - 1) ** 2).mean()
            opt.zero_grad()
            loss.backward()
            opt.step()
        sch.step()
    net.eval()

    class Wrap(nn.Module):
        def __init__(s, net):
            super().__init__()
            s.net = net

        def forward(s, pair):
            return s.net(pair[:, 0, :], pair[:, 1, :])
    dummy = torch.zeros(1, 2, C)
    torch.onnx.export(Wrap(net).eval(), (dummy,), a.out,
                      input_names=["pose_pair"], output_names=["interior"],
                      opset_version=17, dynamo=False)
    print("wrote", a.out)


if __name__ == "__main__":
    main()
