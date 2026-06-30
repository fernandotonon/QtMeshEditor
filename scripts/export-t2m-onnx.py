#!/usr/bin/env python3
# ruff: noqa: E702, E741
"""Train + export a text-to-motion model to ONNX (#411).

ONE-TIME, OFFLINE developer SPIKE tool — NOT shipped; the app never runs Python.
The app would run the resulting t2m.onnx in C++ via ONNX Runtime.

GOAL (de-risk the #411 spike): can a from-scratch model, trained ONLY on
permissively-licensed data (CMU MoCap + its action annotations), generate
recognizable skeletal motion from a text prompt and export to ONNX? Off-the-
shelf models (MDM/T2M-GPT/MotionGPT) are all trained on AMASS-derived
HumanML3D/KIT-ML = NON-COMMERCIAL, so unusable here (see
docs/TEXT_TO_MOTION_SPIKE_411.md). CMU is commercial-OK (same basis as #409 RMIB).

CONTRACT (what the C++ side would consume)
  input  "tokens" float32 [1, V]      bag-of-action-words vector (V = vocab size)
  input  "seed"   float32 [1, Z]      latent noise (Z) for sample variety
  output "motion" float32 [1, T, C]   per-frame canonical pose; C = 22*10 = 220
  Canonical 22-joint skeleton MUST match src/MotionInbetween.cpp kCanonJoints,
  so the existing #409 retargeting adapter maps it onto arbitrary user rigs.

  The text encoder is a TINY bag-of-words over a FIXED action vocabulary (walk,
  run, jump, …) — NOT CLIP. A C++ caller tokenises the prompt against the same
  vocab (shipped as a small JSON) and builds the [1,V] vector. This keeps the
  model CPU-cheap and dependency-free (no 150 MB CLIP encoder), at the cost of
  open-vocabulary range — appropriate for a first --experimental feature.

DATA
  CMU BVH + cmu-mocap-annotations.csv (Motion,Description,Subject). Each clip's
  Description is matched against the action vocab to form its bag-of-words label.

Usage (offline, torch + numpy + bvh + onnx in a venv):
    python export-t2m-onnx.py --bvh <cmu_bvh_dir> --annotations <csv> --out t2m.onnx
"""
import argparse
import csv
import glob
import os
import re

import numpy as np

# Canonical skeleton — MUST match src/MotionInbetween.cpp kCanonJoints + RMIB.
CANON = ["hip", "abdomen", "chest", "neck", "neck1", "head",
         "rcollar", "rshoulder", "relbow", "rhand",
         "lcollar", "lshoulder", "lelbow", "lhand",
         "rbuttock", "rhip", "rknee", "rfoot",
         "lbuttock", "lhip", "lknee", "lfoot"]
DROP = ("jaw", "oris", "tongue", "levator", "special", "eye", "orbicularis",
        "temporalis", "oculi", "risorius", "finger", "metacarpal", "toe",
        "thumb", "__")
J = len(CANON)
C = J * 10
T = 40            # fixed generated clip length (frames)
Z = 16            # latent seed dim

# Fixed action vocabulary — the only words the model understands. Chosen from the
# most-annotated CMU actions. A C++ caller maps prompt words onto this set.
VOCAB = ["walk", "run", "jog", "jump", "dance", "march", "climb", "kick",
         "punch", "sit", "stretch", "throw", "wave", "boxing", "turn", "forward"]


def is_core(name):
    low = name.lower()
    return not any(k in low for k in DROP)


def euler_to_quat_vec(rad, order):
    Tn = rad.shape[0]
    axis_of = {"X": 0, "Y": 1, "Z": 2}
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


def load_annotations(csv_path):
    """motion-id (e.g. '01_01') -> bag-of-words vector over VOCAB."""
    labels = {}
    with open(csv_path, newline="") as f:
        for row in csv.reader(f):
            if len(row) < 2 or not row[0] or row[0] == "Motion":
                continue
            mid, desc = row[0].strip(), row[1].lower()
            vec = np.zeros(len(VOCAB), np.float32)
            for i, w in enumerate(VOCAB):
                if re.search(r"\b" + re.escape(w), desc):
                    vec[i] = 1.0
            if vec.any():
                labels[mid] = vec
    print(f"annotations: {len(labels)} clips matched the action vocab")
    return labels


def motion_id_from_path(path):
    # CMU BVH files are named like '01_01.bvh' (subject_trial).
    base = os.path.basename(path)
    m = re.match(r"(\d+_\d+)", base)
    return m.group(1) if m else None


def preprocess(bvh_dir, labels):
    from bvh import Bvh
    files = sorted(glob.glob(os.path.join(bvh_dir, "**/*.bvh"), recursive=True))
    print(f"found {len(files)} BVH files")
    motions, toks = [], []
    skipped = 0
    for path in files:
        mid = motion_id_from_path(path)
        if mid is None or mid not in labels:
            skipped += 1; continue
        try:
            with open(path) as f:
                m = Bvh(f.read())
        except Exception:
            skipped += 1; continue
        names = [n for n in m.get_joints_names() if is_core(n)]
        if names != CANON:
            skipped += 1; continue
        frames = np.array(m.frames, dtype=np.float32)
        nF = len(frames)
        if nF < T:
            continue
        col, rotcols, roto = 0, {}, {}
        for j in m.get_joints_names():
            chans = m.joint_channels(j); rc, order = [], ""
            for ci, c in enumerate(chans):
                if c.endswith("rotation"):
                    rc.append(col + ci); order += c[0]
            if j in CANON and len(order) == 3:
                rotcols[j], roto[j] = rc, order
            col += len(chans)
        quats = np.zeros((nF, J, 4), np.float32)
        for ji, j in enumerate(CANON):
            if j not in rotcols:
                quats[:, ji, 3] = 1.0; continue
            quats[:, ji] = euler_to_quat_vec(np.deg2rad(frames[:, rotcols[j]]), roto[j])
        # Sample fixed-length windows (stride T//2) — each inherits the clip label.
        for s in range(0, nF - T + 1, max(1, T // 2)):
            motions.append(quats[s:s+T]); toks.append(labels[mid])
    print(f"windows: {len(motions)} (skipped {skipped}/{len(files)} files)")
    if not motions:
        raise SystemExit("No usable labelled windows — check --bvh and --annotations.")
    return np.stack(motions), np.stack(toks)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bvh", required=True)
    ap.add_argument("--annotations", required=True)
    ap.add_argument("--out", default="t2m.onnx")
    ap.add_argument("--epochs", type=int, default=80)
    ap.add_argument("--batch", type=int, default=64)
    a = ap.parse_args()

    import torch
    import torch.nn as nn

    # Cache the (slow) BVH preprocessing so training tweaks don't re-parse 2.5k
    # files each run. Cache keyed by bvh dir + vocab.
    cache = os.path.join(os.path.dirname(a.out) or ".", "t2m_preprocessed.npz")
    if os.path.exists(cache):
        d = np.load(cache)
        mo, tk = d["mo"], d["tk"]
        print(f"loaded cached preprocessing: {mo.shape[0]} windows from {cache}")
    else:
        labels = load_annotations(a.annotations)
        mo, tk = preprocess(a.bvh, labels)
        np.savez(cache, mo=mo, tk=tk)
        print(f"cached preprocessing -> {cache}")

    def pack(q):                       # [...,J,4] quats -> [...,C] (t=0,scale=1)
        sh = q.shape[:-2]
        out = np.zeros(sh + (J, 10), np.float32)
        out[..., 3:7] = q; out[..., 7:10] = 1.0
        return out.reshape(sh + (C,))
    M = torch.tensor(pack(mo))         # [N,T,C]
    Tk = torch.tensor(tk)              # [N,V]
    V = len(VOCAB)
    dev = "mps" if torch.backends.mps.is_available() else "cpu"
    print(f"train N={M.shape[0]} T={T} C={C} V={V} dev={dev}")

    # Motion CVAE (ACTOR/MDM-lite). The earlier naive generator collapsed to a
    # single static mean pose because nothing tied the seed to a SPECIFIC clip —
    # so the loss-optimal output was the dataset mean. Fix:
    #  - ENCODER (train only): reads the real clip -> latent z (mu, logvar). This
    #    gives the seed MEANING (reconstruction pressure), so z carries the
    #    specific motion. At inference z ~ N(0,1).
    #  - TEMPORAL positional encoding on the decoder queries so frames can differ
    #    (the old shared query + uniform conditioning had no temporal signal).
    #  - VELOCITY loss term penalising zero frame-to-frame motion (anti-collapse).
    class PosEnc(nn.Module):
        def __init__(s, d, n):
            super().__init__(); s.pe = nn.Parameter(torch.randn(n, d) * 0.02)
        def forward(s, x): return x + s.pe.unsqueeze(0)[:, :x.shape[1]]

    class CVAE(nn.Module):
        def __init__(s, d=256, layers=4, heads=8):
            super().__init__()
            s.inp = nn.Linear(C, d)
            s.epe = PosEnc(d, T)
            el = nn.TransformerEncoderLayer(d, heads, d*4, batch_first=True, activation="gelu")
            s.encT = nn.TransformerEncoder(el, layers)
            s.tok2mu = nn.Linear(d, Z); s.tok2lv = nn.Linear(d, Z)
            # decoder
            s.cond = nn.Sequential(nn.Linear(V + Z, d), nn.GELU(), nn.Linear(d, d))
            s.dpe = PosEnc(d, T)
            dl = nn.TransformerEncoderLayer(d, heads, d*4, batch_first=True, activation="gelu")
            s.decT = nn.TransformerEncoder(dl, layers)
            s.out = nn.Linear(d, C)
        def encode(s, motion):
            h = s.encT(s.epe(s.inp(motion)))          # [B,T,d]
            g = h.mean(1)                              # pooled clip summary
            return s.tok2mu(g), s.tok2lv(g)
        def decode(s, tokens, z):
            B = tokens.shape[0]
            c = s.cond(torch.cat([tokens, z], -1)).unsqueeze(1)   # [B,1,d]
            q = s.dpe(c.expand(B, T, -1))              # per-frame queries + temporal PE
            return s.out(s.decT(q))                    # [B,T,C]
        def forward(s, tokens, z):                     # inference path (exported)
            return s.decode(tokens, z)

    net = CVAE().to(dev)
    opt = torch.optim.AdamW(net.parameters(), lr=1e-3, weight_decay=1e-5)
    sch = torch.optim.lr_scheduler.CosineAnnealingLR(opt, a.epochs)
    n = M.shape[0]; bs = a.batch

    def qnorm(x):                      # renormalise EACH joint's quaternion block
        # x is [...,C] with C = J*10; the quat is channels 3:7 WITHIN each
        # joint's 10-block, not the flat vector. Reshape to [...,J,10] so every
        # joint is normalised (the earlier version only touched joint 0).
        sh = x.shape[:-1]
        xj = x.reshape(*sh, J, 10)
        pre, q, post = xj[..., :3], xj[..., 3:7], xj[..., 7:]
        qn = q / (q.norm(dim=-1, keepdim=True) + 1e-8)
        return torch.cat([pre, qn, post], dim=-1).reshape(*sh, C)

    def quat_block(x):                 # [...,C] -> [...,J,4] all joints' quats
        sh = x.shape[:-1]
        return x.reshape(*sh, J, 10)[..., 3:7]

    def geo(pred, tgt):                # 1-dot^2 geodesic quat loss, all joints
        dot = (quat_block(qnorm(pred)) * quat_block(tgt)).sum(-1)
        return (1.0 - dot * dot).mean()
    def vel(x):                        # mean frame-to-frame quat change magnitude
        q = quat_block(qnorm(x))
        return (q[:, 1:] - q[:, :-1]).abs().sum(-1).mean()

    for ep in range(a.epochs):
        net.train(); perm = torch.randperm(n)
        tot = vtot = 0.0; nb = 0
        # KL warmup so the decoder first learns to reconstruct, then regularises z.
        beta = min(1.0, ep / max(1, a.epochs * 0.3)) * 1e-3
        for b in range(0, n, bs):
            bi = perm[b:b+bs]
            tokens = Tk[bi].to(dev); tgt = M[bi].to(dev)
            mu, lv = net.encode(tgt)
            z = mu + torch.randn_like(mu) * (0.5 * lv).exp()   # reparam
            pred = net.decode(tokens, z)
            recon = geo(pred, tgt)
            kl = (-0.5 * (1 + lv - mu*mu - lv.exp())).mean()
            # velocity matching: generated motion magnitude should match target's
            # (anti-collapse — a static output is heavily penalised here).
            vloss = (vel(pred) - vel(tgt)).abs()
            loss = recon + beta * kl + 0.5 * vloss
            opt.zero_grad(); loss.backward(); opt.step()
            tot += recon.item(); vtot += vel(pred).item(); nb += 1
        sch.step()
        if ep % 5 == 0 or ep == a.epochs - 1:
            print(f"ep{ep:3d} recon{tot/max(1,nb):.5f} genvel{vtot/max(1,nb):.4f} (real~0.14)")

    net.eval().cpu()
    class Wrap(nn.Module):
        def __init__(s, g): super().__init__(); s.g = g
        def forward(s, tokens, seed):
            return qnorm(s.g(tokens, seed))
    dummy = (torch.zeros(1, V), torch.zeros(1, Z))
    torch.onnx.export(
        Wrap(net).eval(), dummy, a.out,
        input_names=["tokens", "seed"], output_names=["motion"],
        dynamic_axes={"motion": {0: "B"}}, opset_version=17, dynamo=False)
    print("wrote", a.out)
    # also dump the vocab so the C++ side tokenises identically
    import json
    with open(os.path.splitext(a.out)[0] + "-vocab.json", "w") as f:
        json.dump({"vocab": VOCAB, "Z": Z, "T": T, "C": C, "J": J}, f)
    print("wrote vocab json")


if __name__ == "__main__":
    main()
