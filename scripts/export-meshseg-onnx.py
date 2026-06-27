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

Usage (offline, with torch + numpy + onnx in a venv):
    python export-meshseg-onnx.py --samples 4000 --points 4096 --out meshseg.onnx
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
    torsoH = rng.uniform(0.34, 0.46); torsoW = rng.uniform(0.14, 0.20)
    torsoD = rng.uniform(0.08, 0.12); headR = rng.uniform(0.08, 0.12)
    armLen = rng.uniform(0.30, 0.42); armR = rng.uniform(0.035, 0.06)
    legLen = rng.uniform(0.38, 0.50); legR = rng.uniform(0.05, 0.08)
    shoulderX = torsoW + armR
    pts, lab = [], []
    def add(p, l): pts.append(p); lab.append(np.full(len(p), l, np.int64))
    add(_box(np.array([0, torsoH/2, 0]), np.array([torsoW, torsoH/2, torsoD]), npp, rng), TORSO)
    add(_sphere(np.array([0, torsoH + headR*1.1, 0]), headR, npp, rng), HEAD)
    drop = rng.uniform(0.6, 1.0)
    for sx, l in ((+1, RARM), (-1, LARM)):
        p0 = np.array([sx*shoulderX, torsoH, 0])
        p1 = np.array([sx*(shoulderX + armLen*0.4*(1-drop)), torsoH - armLen*drop, rng.uniform(-0.1, 0.1)])
        add(_capsule(p0, p1, armR, npp, rng), l)
    for sx, l in ((+1, RLEG), (-1, LLEG)):
        p0 = np.array([sx*torsoW*0.5, 0.0, 0])
        p1 = np.array([sx*torsoW*0.5, -legLen, rng.uniform(-0.05, 0.05)])
        add(_capsule(p0, p1, legR, npp, rng), l)
    P = np.concatenate(pts).astype(np.float32); L = np.concatenate(lab)
    if rng.random() < 0.7:
        ang = rng.uniform(-0.4, 0.4, size=3)
        cx, cy, cz = np.cos(ang); sx_, sy, sz = np.sin(ang)
        Rx = np.array([[1,0,0],[0,cx,-sx_],[0,sx_,cx]]); Ry = np.array([[cy,0,sy],[0,1,0],[-sy,0,cy]])
        Rz = np.array([[cz,-sz,0],[sz,cz,0],[0,0,1]])
        P = P @ (Rx @ Ry @ Rz).T.astype(np.float32)
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


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--samples", type=int, default=4000)
    ap.add_argument("--points", type=int, default=4096)
    ap.add_argument("--epochs", type=int, default=40)
    ap.add_argument("--out", default="meshseg.onnx")
    ap.add_argument("--seed", type=int, default=0)
    a = ap.parse_args()

    import torch
    import torch.nn as nn

    print("generating synthetic data…")
    P, L = gen_dataset(a.samples, a.points, a.seed)
    P = torch.tensor(P); L = torch.tensor(L)
    n = P.shape[0]; N = P.shape[1]
    nval = max(1, n // 10)
    dev = "mps" if torch.backends.mps.is_available() else "cpu"
    print(f"data N={n} points={N} dev={dev}")

    class PointSeg(nn.Module):
        def __init__(s, d=128):
            super().__init__()
            s.mlp1 = nn.Sequential(nn.Linear(3,64), nn.GELU(), nn.Linear(64,d), nn.GELU())
            s.mlp2 = nn.Sequential(nn.Linear(d,d), nn.GELU(), nn.Linear(d,d), nn.GELU())
            s.head = nn.Sequential(nn.Linear(d+d,d), nn.GELU(), nn.Linear(d,C))
        def forward(s, pts):
            f = s.mlp1(pts); g = s.mlp2(f)
            glob = g.max(dim=1, keepdim=True).values.expand(-1, g.shape[1], -1)
            return s.head(torch.cat([f, glob], dim=-1))

    net = PointSeg().to(dev)
    opt = torch.optim.AdamW(net.parameters(), lr=2e-3, weight_decay=1e-4)
    sch = torch.optim.lr_scheduler.CosineAnnealingLR(opt, a.epochs)
    lossf = nn.CrossEntropyLoss()
    bs = 32
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
            with torch.no_grad():
                acc = (net(P[:nval].to(dev)).argmax(-1) == L[:nval].to(dev)).float().mean().item()
            print(f"ep{ep:3d} loss{loss.item():.4f} val_pt_acc{acc:.4f}")

    net.eval().cpu()
    torch.onnx.export(
        net, (torch.zeros(1, N, 3),), a.out,
        input_names=["points"], output_names=["logits"],
        dynamic_axes={"points": {1: "N"}, "logits": {1: "N"}},
        opset_version=17, dynamo=False)
    print("wrote", a.out)


if __name__ == "__main__":
    main()
