#!/usr/bin/env python3
# ruff: noqa: E702, E741
#   This is a compact, offline, NOT-shipped dev tool: a few `;`-joined helper
#   one-liners (E702) and the `l` part-label loop var (E741) are intentional for
#   density. The app never runs this file.
"""Train + export the mesh part-segmentation model to ONNX (#410).

ONE-TIME, OFFLINE developer tool — NOT shipped with the app, and the app never
runs Python. The app runs the resulting meshseg.onnx in C++ via ONNX Runtime
(src/MeshSegmenter.cpp), downloading it on first use to
AppData/ai_models/segment/meshseg.onnx.

WHAT IT PRODUCES
  meshseg.onnx with the contract MeshSegmenter::predict() expects:
    input  "points" float32 [1, N, 3]   (point cloud, centred unit box)
    output "logits" float32 [1, N, C]   (per-point class logits; C = 7)
  Part order MUST match src/MeshSegmenter.h:
    0 unknown, 1 head, 2 torso, 3 left_arm, 4 right_arm, 5 left_leg, 6 right_leg.

DATA — SYNTHETIC / CC0 (no external dataset)
  The standard part-seg datasets (ShapeNet-Part, PartNet) are NON-COMMERCIAL, so
  they can't train a model we ship under the project's permissive bar (same wall
  as #408 RigNet / #409 LAFAN1). Instead we SYNTHESISE labeled humanoids from
  primitive parts (head sphere, torso box, four limb capsules) in randomised
  proportions / poses / orientations — because WE place each part, the per-point
  label is known by construction. The data + labels are ours (CC0), so the
  trained weights are freely redistributable. The model learns the spatial
  configuration of a humanoid and transfers to real humanoid meshes (validated:
  the shipped model labels a Mixamo character into symmetric head/torso/arms/
  legs, an improvement over the geometric fallback).

MODEL
  A small PointNet-style segmenter: shared per-point MLP → max-pooled global
  feature → concat back to each point → per-point classifier. ~0.3 MB ONNX.

MINED REAL DATA (continual improvement)
  Synthetic primitives can't capture real surface distributions (dense face
  meshes, true limb shapes, animal topology). The app can mine EXACT labels
  from any RIGGED mesh for free — `qtmesh segment <mesh> --dump-training-data
  out.json` reads per-vertex labels from the rig (bone weights → bone name →
  part). Pass those JSONs via `--real-data` to MIX them with the synthetic set;
  the model improves as more rigged assets are mined, and the gains land on the
  MODEL path used for UNrigged meshes (rigged meshes already use the exact
  rig-prior path in-app). This is the "train further as we gather data" loop.

Usage (offline, with torch + numpy + onnx in a venv):
    python export-meshseg-onnx.py --samples 4000 --points 4096 --out meshseg.onnx
    # mix in mined real meshes:
    python export-meshseg-onnx.py --samples 6000 --real-data ./mined/ --out meshseg.onnx
"""
import argparse
import numpy as np

HEAD, TORSO, LARM, RARM, LLEG, RLEG = 1, 2, 3, 4, 5, 6
C = 7  # Part::Count


# --- synthetic humanoid generation -----------------------------------------
def _box(c, half, n, rng):  return c + (rng.random((n, 3)) * 2 - 1) * half
def _sphere(c, r, n, rng):
    v = rng.normal(size=(n, 3)); v /= np.linalg.norm(v, axis=1, keepdims=True) + 1e-9
    return c + v * (r * np.cbrt(rng.random((n, 1))))
def _capsule(p0, p1, r, n, rng):
    t = rng.random((n, 1)); axis = p0 * (1 - t) + p1 * t
    off = rng.normal(size=(n, 3)); off /= np.linalg.norm(off, axis=1, keepdims=True) + 1e-9
    return axis + off * (r * np.cbrt(rng.random((n, 1))))


def make_humanoid(rng, npp=700):
    # Wider proportion variety so stubby / lanky / big-headed characters are all
    # covered (the real meshes range from a chunky man to a big-eared cat).
    torsoH = rng.uniform(0.28, 0.50); torsoW = rng.uniform(0.12, 0.24)
    torsoD = rng.uniform(0.07, 0.14); headR = rng.uniform(0.07, 0.18)
    armLen = rng.uniform(0.26, 0.46); armR = rng.uniform(0.030, 0.07)
    legLen = rng.uniform(0.30, 0.55); legR = rng.uniform(0.045, 0.09)
    shoulderX = torsoW + armR
    shoulderY = torsoH * rng.uniform(0.82, 0.98)
    pts, lab = [], []
    def add(p, l): pts.append(p); lab.append(np.full(len(p), l, np.int64))

    add(_box(np.array([0, torsoH/2, 0]), np.array([torsoW, torsoH/2, torsoD]), npp, rng), TORSO)

    headC = np.array([0, torsoH + headR*1.05, 0])
    add(_sphere(headC, headR, npp, rng), HEAD)
    # Optional protrusions on the HEAD (ears / hat / muzzle): they belong to the
    # head, so labelling them HEAD teaches the model NOT to call lateral
    # head-level bumps "arm" (the cat-ear → arm bug). A fraction of head points.
    if rng.random() < 0.6:
        for sx in (+1, -1):
            if rng.random() < 0.85:
                ear = headC + np.array([sx*headR*0.9, headR*rng.uniform(0.4, 1.1), 0])
                add(_sphere(ear, headR*rng.uniform(0.3, 0.6), npp//3, rng), HEAD)

    # ARM POSE: dominant T/A-pose (arms OUT to the sides) — this is what real
    # rigged characters use and is exactly what the old model (arms-down only)
    # got wrong. `outFrac`=1 → horizontal T-pose; smaller → A-pose; rare → down.
    roll = rng.random()
    outFrac = 1.0 if roll < 0.55 else (rng.uniform(0.45, 0.9) if roll < 0.9 else rng.uniform(0.0, 0.3))
    for sx, l in ((+1, RARM), (-1, LARM)):
        p0 = np.array([sx*shoulderX, shoulderY, 0])
        # End point: reach out by `outFrac` of armLen laterally, drop the rest.
        p1 = np.array([sx*(shoulderX + armLen*outFrac),
                       shoulderY - armLen*(1.0-outFrac),
                       rng.uniform(-0.06, 0.06)])
        add(_capsule(p0, p1, armR, npp, rng), l)

    for sx, l in ((+1, RLEG), (-1, LLEG)):
        p0 = np.array([sx*torsoW*0.5, 0.0, 0])
        p1 = np.array([sx*torsoW*rng.uniform(0.4, 0.7), -legLen, rng.uniform(-0.05, 0.05)])
        add(_capsule(p0, p1, legR, npp, rng), l)

    P = np.concatenate(pts).astype(np.float32); L = np.concatenate(lab)

    # Augmentation: FULL 360° yaw about the up axis so the model is invariant to
    # which way the character faces (a real mesh can face any compass direction;
    # the old ±0.25rad yaw made the model brittle — a 180°-yawed mesh got its
    # left/right mirrored and head mislabelled). We keep it UPRIGHT (only a tiny
    # tilt) and DO NOT mirror, so true left/right handedness is preserved.
    yaw = rng.uniform(0.0, 2*np.pi)
    tilt = rng.uniform(-0.08, 0.08, size=2)
    cy, sy = np.cos(yaw), np.sin(yaw)
    Ry = np.array([[cy,0,sy],[0,1,0],[-sy,0,cy]])
    cx, sx_ = np.cos(tilt[0]), np.sin(tilt[0]); cz, sz = np.cos(tilt[1]), np.sin(tilt[1])
    Rx = np.array([[1,0,0],[0,cx,-sx_],[0,sx_,cx]]); Rz = np.array([[cz,-sz,0],[sz,cz,0],[0,0,1]])
    P = P @ (Ry @ Rx @ Rz).T.astype(np.float32)
    # Per-point jitter so the model doesn't overfit clean primitive surfaces.
    P += rng.normal(scale=0.004, size=P.shape).astype(np.float32)

    centre = 0.5*(P.min(0)+P.max(0)); half = float(np.max(0.5*(P.max(0)-P.min(0))))
    return (P - centre) / (half + 1e-9), L


def gen_dataset(samples, points, seed):
    rng = np.random.default_rng(seed)
    aP = np.zeros((samples, points, 3), np.float32)
    aL = np.zeros((samples, points), np.int64)
    for i in range(samples):
        P, L = make_humanoid(rng)
        idx = rng.choice(len(P), points, replace=len(P) < points)
        aP[i] = P[idx]; aL[i] = L[idx]
    return aP, aL


# --- mined real meshes (rig-prior ground truth) ----------------------------
# Each JSON is written by `qtmesh segment <mesh> --dump-training-data out.json`
# (schema "qtmesh-meshseg-training-v1"): a normalised point cloud + EXACT
# per-vertex part labels read from the mesh's rig (bone weights -> bone name ->
# part). Every rigged asset = one free, exactly-labelled sample. We resample
# each to `points` and apply the SAME small upright augmentation as the
# synthetic data + a few aug copies, so a handful of real meshes still teaches
# the model real-world surface distributions (dense faces, true limb shapes)
# that synthetic primitives can't.
def _augment_upright(P, rng):
    yaw = rng.uniform(0.0, 2*np.pi)   # full 360° facing (see make_humanoid note)
    tilt = rng.uniform(-0.08, 0.08, size=2)
    cy, sy = np.cos(yaw), np.sin(yaw)
    Ry = np.array([[cy, 0, sy], [0, 1, 0], [-sy, 0, cy]])
    cx, sx = np.cos(tilt[0]), np.sin(tilt[0]); cz, sz = np.cos(tilt[1]), np.sin(tilt[1])
    Rx = np.array([[1, 0, 0], [0, cx, -sx], [0, sx, cx]])
    Rz = np.array([[cz, -sz, 0], [sz, cz, 0], [0, 0, 1]])
    out = P @ (Ry @ Rx @ Rz).T.astype(np.float32)
    out += rng.normal(scale=0.004, size=out.shape).astype(np.float32)
    return out.astype(np.float32)


def load_real_data(paths, points, aug, seed):
    import glob
    import json
    import os
    rng = np.random.default_rng(seed + 777)
    files = []
    for pth in paths:
        files += sorted(glob.glob(os.path.join(pth, "*.json"))) if os.path.isdir(pth) else [pth]
    if not files:
        return np.zeros((0, points, 3), np.float32), np.zeros((0, points), np.int64)
    Ps, Ls = [], []
    for f in files:
        d = json.load(open(f))
        if d.get("schema") != "qtmesh-meshseg-training-v1":
            print(f"  skip {f}: unexpected schema {d.get('schema')!r}"); continue
        P = np.asarray(d["points"], np.float32).reshape(-1, 3)
        L = np.asarray(d["labels"], np.int64)
        if len(P) != len(L) or len(P) == 0:
            print(f"  skip {f}: points/labels mismatch"); continue
        # base sample + `aug` augmented copies (yaw/tilt/jitter).
        for k in range(1 + aug):
            idx = rng.choice(len(P), points, replace=len(P) < points)
            pp = P[idx]
            if k > 0:
                pp = _augment_upright(pp, rng)
                # re-centre + re-scale to unit box after rotation.
                c = 0.5 * (pp.min(0) + pp.max(0)); h = float(np.max(0.5 * (pp.max(0) - pp.min(0))))
                pp = (pp - c) / (h + 1e-9)
            Ps.append(pp.astype(np.float32)); Ls.append(L[idx])
    print(f"loaded {len(files)} real mesh file(s) -> {len(Ps)} samples (incl. {aug}x aug)")
    return np.stack(Ps), np.stack(Ls)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--samples", type=int, default=6000)
    ap.add_argument("--points", type=int, default=1024)   # kNN cdist is O(N^2); keep modest
    ap.add_argument("--epochs", type=int, default=60)
    ap.add_argument("--batch", type=int, default=8)        # cdist+gather are memory-heavy
    ap.add_argument("--out", default="meshseg.onnx")
    ap.add_argument("--seed", type=int, default=0)
    ap.add_argument("--real-data", nargs="*", default=[],
                    help="dirs/files of `qtmesh segment --dump-training-data` JSON "
                         "samples (rig-prior ground truth) to MIX with synthetic data")
    ap.add_argument("--real-aug", type=int, default=8,
                    help="augmented copies per real mesh (yaw/tilt/jitter)")
    ap.add_argument("--real-weight", type=float, default=1.0,
                    help="oversample factor for real samples so they aren't drowned "
                         "out by synthetic. 0 = auto-balance to ~50/50 real:synthetic")
    ap.add_argument("--dim", type=int, default=128, help="model feature width")
    ap.add_argument("--knn", type=int, default=12, help="local kNN neighbours")
    a = ap.parse_args()

    import torch
    import torch.nn as nn

    print("generating synthetic data…")
    P, L = gen_dataset(a.samples, a.points, a.seed)
    # `src`: 0 = synthetic, 1 = real — kept so we can report val accuracy PER
    # SOURCE (real-mesh accuracy is what actually matters; a synthetic-only
    # average hides it).
    src = np.zeros(len(P), np.int64)
    if a.real_data:
        rP, rL = load_real_data(a.real_data, a.points, a.real_aug, a.seed)
        if len(rP):
            # Oversample real so it isn't drowned out by synthetic. With the
            # default (auto, --real-weight 0) we replicate real to ~match the
            # synthetic count (≈50/50); the per-mesh augmentation already makes
            # the copies non-identical, and we re-augment replicas below.
            reps = a.real_weight
            if reps <= 0:
                reps = max(1.0, len(P) / max(1, len(rP)))
            reps_i = int(round(reps))
            if reps_i > 1:
                rng = np.random.default_rng(a.seed + 2)
                extraP, extraL = [], []
                for _ in range(reps_i - 1):
                    for i in range(len(rP)):
                        extraP.append(_augment_upright(rP[i], rng)); extraL.append(rL[i])
                rP = np.concatenate([rP, np.stack(extraP)])
                rL = np.concatenate([rL, np.stack(extraL)])
            P = np.concatenate([P, rP]); L = np.concatenate([L, rL])
            src = np.concatenate([src, np.ones(len(rP), np.int64)])
            sh = np.random.default_rng(a.seed + 1).permutation(len(P))
            P = P[sh]; L = L[sh]; src = src[sh]
            print(f"combined dataset: {len(P)} samples "
                  f"({a.samples} synthetic + {len(rP)} real @ {reps_i}x = "
                  f"{100*len(rP)/len(P):.0f}% real)")
    P = torch.tensor(P); L = torch.tensor(L)
    srcT = torch.tensor(src)
    n = P.shape[0]; N = P.shape[1]
    nval = max(1, n // 10)
    dev = "mps" if torch.backends.mps.is_available() else "cpu"
    print(f"data N={n} points={N} dev={dev} dim={a.dim} knn={a.knn}")

    class PointSeg(nn.Module):
        # Classic PointNet SEGMENTATION network — deliberately uses ONLY ops that
        # every ONNX Runtime build executes identically (Linear / GELU / max).
        #
        # NO in-graph kNN (cdist / topk / gather): the prebuilt
        # onnxruntime-osx-universal2 binary the app links MIS-EXECUTES those ops
        # on arm64 (verified: Python ORT 1.20.1 gives 0.98 on the same model +
        # input, the C++ universal2 build gives 0.33). A flat PointNet sidesteps
        # the broken kernels entirely and runs correctly in the app.
        #
        # Local context is recovered the PointNet-seg way: per-point features are
        # concatenated with the GLOBAL max-pooled feature (broadcast back to every
        # point), so each point classifies in the context of the whole shape. `k`
        # is accepted but unused (kept for CLI compat).
        def __init__(s, d=128, k=12):
            super().__init__()
            s.mlp1 = nn.Sequential(nn.Linear(3, 64), nn.GELU(), nn.Linear(64, d), nn.GELU())
            s.mlp2 = nn.Sequential(nn.Linear(d, d), nn.GELU(), nn.Linear(d, d), nn.GELU())
            # segmentation head: [per-point f ; per-point g ; global maxpool(g)]
            s.head = nn.Sequential(nn.Linear(d + d + d, d), nn.GELU(),
                                   nn.Linear(d, d), nn.GELU(), nn.Linear(d, C))

        def forward(s, pts):                      # pts: [B,N,3]
            f = s.mlp1(pts)                        # [B,N,d]
            g = s.mlp2(f)                          # [B,N,d]
            B, N, _ = f.shape
            glob = g.max(dim=1, keepdim=True).values.expand(-1, N, -1)  # global ctx
            return s.head(torch.cat([f, g, glob], dim=-1))

    net = PointSeg(d=a.dim, k=a.knn).to(dev)
    opt = torch.optim.AdamW(net.parameters(), lr=2e-3, weight_decay=1e-4)
    sch = torch.optim.lr_scheduler.CosineAnnealingLR(opt, a.epochs)
    lossf = nn.CrossEntropyLoss()
    bs = a.batch
    for ep in range(a.epochs):
        net.train()
        perm = torch.randperm(n - nval) + nval
        for b in range(0, perm.numel(), bs):
            bi = perm[b:b+bs]
            logits = net(P[bi].to(dev))
            loss = lossf(logits.reshape(-1, C), L[bi].to(dev).reshape(-1))
            opt.zero_grad(); loss.backward(); opt.step()
        sch.step()
        if ep % 5 == 0 or ep == a.epochs - 1:
            net.eval()
            # Report val accuracy split by source — real-mesh accuracy is the
            # number that matters; a blended average hides it.
            cor = {0: 0, 1: 0}; tot = {0: 0, 1: 0}
            with torch.no_grad():
                for b in range(0, nval, bs):     # batch val too (cdist is O(N^2) memory)
                    pv = P[b:b+bs].to(dev); lv = L[b:b+bs].to(dev)
                    sv = srcT[b:b+bs]
                    ok = (net(pv).argmax(-1) == lv)
                    for srcid in (0, 1):
                        m = (sv == srcid)
                        if m.any():
                            cor[srcid] += ok[m.to(dev)].sum().item()
                            tot[srcid] += int(m.sum().item()) * lv.shape[1]
            syn = cor[0] / max(1, tot[0]); real = cor[1] / max(1, tot[1])
            allc = (cor[0] + cor[1]) / max(1, tot[0] + tot[1])
            print(f"ep{ep:3d} loss{loss.item():.4f} val_acc{allc:.4f} "
                  f"synth{syn:.4f} real{real:.4f}(n={tot[1]//max(1,N)})")

    net.eval().cpu()
    torch.onnx.export(
        net, (torch.zeros(1, N, 3),), a.out,
        input_names=["points"], output_names=["logits"],
        dynamic_axes={"points": {1: "N"}, "logits": {1: "N"}},
        opset_version=17, dynamo=False)
    print("wrote", a.out)


if __name__ == "__main__":
    main()
