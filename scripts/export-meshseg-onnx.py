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
  Frame convention (matches the app's inference path): +Y up, character
  facing +Z, LEFT limbs at +X. MeshSegmenter::predict() remaps the user's
  --up-axis onto Y; facing is learned (feet/muzzle point +Z in training, with
  yaw augmentation for robustness).

DATA — SYNTHETIC (CC0, ours) + MINED CC0 RIGS
  The standard part-seg datasets (ShapeNet-Part, PartNet) are NON-COMMERCIAL, so
  they can't train a model we ship under the project's permissive bar (same wall
  as #408 RigNet / #409 LAFAN1). Two permissive sources instead:

  1. SYNTHETIC bodies (v2): SURFACE-sampled (real mesh vertices live on
     surfaces, not in volumes — the v1 volumetric blobs were the main domain
     gap), CONNECTED part layouts in three body plans — humanoid (incl. chibi
     big-head/big-ear cartoon proportions, the failure case that motivated v2),
     quadruped (all four legs labelled by SIDE, matching the rig-prior
     convention where "FrontLeftLeg"→left_leg), and biped-with-tail (dino).
     Feet/muzzles point +Z so the model can learn facing. Per-part point
     DENSITY is randomised (real characters put 30-50%% of vertices in the
     head/face). Because WE place each part, per-point labels are exact and the
     data is ours (CC0) — the trained weights are freely redistributable.

  2. MINED REAL RIGS: `qtmesh segment <mesh> --dump-training-data out.json`
     reads EXACT per-vertex labels from a rigged mesh (bone weights → bone name
     → part). Only CC0/CC-BY sources are allowed (see
     scripts/fetch-training-rigs.sh + the SOURCES.md ledger it maintains).
     Mined clouds are in ARBITRARY frames (FBX bind poses are often Z-up or
     side-facing), so the loader CANONICALISES each cloud from its own labels:
     up = legs→torso direction, left = right-arm→left-arm (or right-leg→
     left-leg) direction, forward = left×up. It also REASSIGNS the left/right
     side of arm/leg labels geometrically — the miner's bone-name side
     detection is unreliable on some rigs (up to ~30%% wrong-side points), but
     in a bind pose side is purely geometric. Label 0 (unknown) is masked out
     of the loss.

MODEL
  A PointNet++-style segmenter: shared per-point MLP, TWO kNN local-aggregation
  blocks (neighbourhoods from one in-graph cdist+topk — exportable to ONNX),
  global max-pool feature, per-point classifier. ~1 MB ONNX.

TRAINING SCHEDULE
  Phase 1 trains on random 2048-point subsets (fast, and subsetting is free
  augmentation); phase 2 fine-tunes at the app's inference size of 4096 points
  so the kNN density statistics match what MeshSegmenter::predict() feeds the
  model (Options::samplePoints = 4096, duplicate-padded for small meshes).

Usage (offline, with torch + numpy + onnx in a venv):
    python export-meshseg-onnx.py --samples 4000 --out meshseg.onnx
    # mix in mined real meshes (dirs of --dump-training-data JSONs):
    python export-meshseg-onnx.py --samples 4000 --real-data ./mined/ \
        --val-real Male_Suit Female_Dress Trex --out meshseg.onnx
    # sanity-check the canonicalisation of mined data without training:
    python export-meshseg-onnx.py --real-data ./mined/ --check-real
"""
import argparse
import glob
import json
import os

import numpy as np

HEAD, TORSO, LARM, RARM, LLEG, RLEG = 1, 2, 3, 4, 5, 6
C = 7  # Part::Count
N_BASE = 4096  # stored points per sample == the app's inference sample size


# --- surface samplers --------------------------------------------------------
def _unit_dirs(n, rng):
    v = rng.normal(size=(n, 3)); v /= np.linalg.norm(v, axis=1, keepdims=True) + 1e-9
    return v


def sphere_surf(c, r, n, rng, squash=None):
    """Points on an ellipsoid surface (r scalar radius, squash per-axis scale)."""
    p = _unit_dirs(n, rng) * r
    if squash is not None: p = p * squash
    return c + p


def capsule_surf(p0, p1, r, n, rng):
    """Points on a capsule surface (cylinder side + spherical caps)."""
    p0 = np.asarray(p0, float); p1 = np.asarray(p1, float)
    axis = p1 - p0; L = np.linalg.norm(axis) + 1e-9; u = axis / L
    # area split: side 2πrL vs caps 4πr²
    side_frac = L / (L + 2 * r)
    ns = int(n * side_frac); nc = n - ns
    # side: random t along axis, random dir ⊥ axis
    t = rng.random((ns, 1))
    d = _unit_dirs(ns, rng); d -= (d @ u)[:, None] * u
    d /= np.linalg.norm(d, axis=1, keepdims=True) + 1e-9
    side = p0 + t * axis + d * r
    caps_dir = _unit_dirs(nc, rng)
    which = rng.random(nc) < 0.5
    caps = np.where(which[:, None], p0 + caps_dir * r, p1 + caps_dir * r)
    return np.concatenate([side, caps])


def box_surf(c, half, n, rng):
    """Points on a box surface, faces weighted by area."""
    half = np.asarray(half, float)
    areas = np.array([half[1] * half[2], half[0] * half[2], half[0] * half[1]])
    areas = np.repeat(areas, 2); areas /= areas.sum()
    face = rng.choice(6, size=n, p=areas)
    p = (rng.random((n, 3)) * 2 - 1) * half
    ax = face // 2; sign = np.where(face % 2 == 0, 1.0, -1.0)
    p[np.arange(n), ax] = sign * half[ax]
    return c + p


# --- synthetic body plans (frame: +Y up, facing +Z, LEFT at +X) --------------
def _pose_arm(shoulder, sx, armLen, armR, rng):
    """Return (segments, hand_centre) for one arm. sx=+1 → LEFT."""
    roll = rng.random()
    if roll < 0.45:        # T-pose
        d1 = np.array([sx, rng.uniform(-0.15, 0.15), rng.uniform(-0.1, 0.1)])
    elif roll < 0.80:      # A-pose, 30-70 deg down
        a = rng.uniform(0.5, 1.2)
        d1 = np.array([sx * np.cos(a), -np.sin(a), rng.uniform(-0.1, 0.1)])
    elif roll < 0.92:      # arms down
        d1 = np.array([sx * rng.uniform(0.05, 0.25), -1.0, rng.uniform(-0.1, 0.1)])
    else:                  # zombie forward
        d1 = np.array([sx * rng.uniform(0.0, 0.2), rng.uniform(-0.2, 0.2), 1.0])
    d1 /= np.linalg.norm(d1)
    elbow = shoulder + d1 * armLen * 0.5
    bend = rng.uniform(0, 0.6)
    d2 = d1 + np.array([0, -bend * 0.4, bend * rng.uniform(-0.5, 1.0)])
    d2 /= np.linalg.norm(d2)
    wrist = elbow + d2 * armLen * 0.5
    return [(shoulder, elbow), (elbow, wrist)], wrist


def make_humanoid(rng):
    """Surface-sampled connected humanoid. Returns list of (points, label, weight)."""
    regime = rng.choice(['normal', 'chibi', 'lanky'], p=[0.5, 0.3, 0.2])
    if regime == 'chibi':
        legL = rng.uniform(0.10, 0.30); torsoH = rng.uniform(0.15, 0.35)
        headR = rng.uniform(0.55, 1.4) * torsoH
    elif regime == 'lanky':
        legL = rng.uniform(0.45, 0.65); torsoH = rng.uniform(0.30, 0.50)
        headR = rng.uniform(0.18, 0.35) * torsoH
    else:
        legL = rng.uniform(0.30, 0.55); torsoH = rng.uniform(0.28, 0.50)
        headR = rng.uniform(0.25, 0.55) * torsoH
    torsoW = torsoH * rng.uniform(0.35, 0.65)
    torsoD = torsoW * rng.uniform(0.45, 0.85)
    armLen = (torsoH + legL) * rng.uniform(0.35, 0.60)
    armR = torsoW * rng.uniform(0.18, 0.40)
    legR = torsoW * rng.uniform(0.25, 0.50)

    parts = []  # (sampler(n)->pts, label, area_weight)
    pel = legL
    torsoC = np.array([0, pel + torsoH / 2, 0])
    if rng.random() < 0.5:
        parts.append((lambda n, c=torsoC, h=np.array([torsoW, torsoH / 2, torsoD]):
                      box_surf(c, h, n, rng), TORSO, torsoH * torsoW * 4))
    else:
        parts.append((lambda n, c=torsoC, r=torsoH / 2,
                      s=np.array([torsoW / (torsoH / 2), 1, torsoD / (torsoH / 2)]):
                      sphere_surf(c, r, n, rng, s), TORSO, torsoH * torsoW * 4))

    # neck + head (both HEAD — bone-name convention maps neck→head)
    neckL = torsoH * rng.uniform(0.05, 0.25)
    neckTop = np.array([0, pel + torsoH + neckL, 0])
    parts.append((lambda n, a=np.array([0, pel + torsoH, 0]), b=neckTop,
                  r=headR * rng.uniform(0.25, 0.5):
                  capsule_surf(a, b, r, n, rng), HEAD, neckL * headR * 2))
    headC = neckTop + np.array([0, headR * rng.uniform(0.75, 1.0), 0])
    hsq = np.array([rng.uniform(0.8, 1.2), rng.uniform(0.8, 1.25), rng.uniform(0.75, 1.1)])
    parts.append((lambda n, c=headC, r=headR, s=hsq:
                  sphere_surf(c, r, n, rng, s), HEAD, headR * headR * 8))
    if rng.random() < 0.55:      # ears / horns / hat — lateral or top head bumps
        for sx in (+1, -1):
            if rng.random() < 0.85:
                er = headR * rng.uniform(0.25, 0.7)
                ec = headC + np.array([sx * headR * rng.uniform(0.7, 1.0),
                                       headR * rng.uniform(0.3, 1.1), 0])
                parts.append((lambda n, c=ec, r=er: sphere_surf(c, r, n, rng),
                              HEAD, er * er * 6))
    if rng.random() < 0.35:      # muzzle / nose — a FORWARD (+Z) head bump
        mc = headC + np.array([0, -headR * 0.2, headR * rng.uniform(0.7, 1.0)])
        mr = headR * rng.uniform(0.25, 0.5)
        parts.append((lambda n, c=mc, r=mr: sphere_surf(c, r, n, rng), HEAD, mr * mr * 6))

    shoulderY = pel + torsoH * rng.uniform(0.82, 1.0)
    for sx, l in ((+1, LARM), (-1, RARM)):        # LEFT at +X (rig-prior convention)
        sh = np.array([sx * (torsoW + armR * 0.5), shoulderY, 0])
        segs, wrist = _pose_arm(sh, sx, armLen, armR, rng)
        for a, b in segs:
            parts.append((lambda n, a=a, b=b, r=armR: capsule_surf(a, b, r, n, rng),
                          l, armLen * armR))
        handR = armR * rng.uniform(1.1, 1.7)
        parts.append((lambda n, c=wrist, r=handR: sphere_surf(c, r, n, rng),
                      l, handR * handR * 5))

    stance = rng.uniform(0.0, 0.35)
    for sx, l in ((+1, LLEG), (-1, RLEG)):
        hip = np.array([sx * torsoW * rng.uniform(0.4, 0.75), pel, 0])
        ankle = hip + np.array([sx * stance * legL, -legL, rng.uniform(-0.05, 0.05)])
        parts.append((lambda n, a=hip, b=ankle, r=legR: capsule_surf(a, b, r, n, rng),
                      l, legL * legR))
        # foot box pointing FORWARD (+Z) — the model's main facing cue
        footL = legR * rng.uniform(1.6, 3.0)
        fc = ankle + np.array([0, -legR * 0.4, footL * 0.5])
        parts.append((lambda n, c=fc, h=np.array([legR * 0.9, legR * 0.5, footL]):
                      box_surf(c, h, n, rng), l, footL * legR * 3))
    return parts


def make_quadruped(rng):
    """Four-legged body plan: horizontal torso along Z, head at +Z. All four
    legs are labelled by SIDE only (left_leg/right_leg) — matching the
    rig-prior convention (partForBoneName maps any '*leg/paw/hoof' by side)."""
    parts = []
    bodyL = rng.uniform(0.5, 1.0); bodyR = rng.uniform(0.14, 0.30)
    legL = rng.uniform(0.7, 1.6) * bodyR; legR = bodyR * rng.uniform(0.2, 0.45)
    y0 = legL + bodyR   # torso axis height
    b0 = np.array([0, y0, -bodyL / 2]); b1 = np.array([0, y0, bodyL / 2])
    parts.append((lambda n, a=b0, b=b1, r=bodyR: capsule_surf(a, b, r, n, rng),
                  TORSO, bodyL * bodyR * 3))
    # neck + head at +Z, raised
    headR = bodyR * rng.uniform(0.5, 1.0)
    neckA = rng.uniform(0.3, 1.2)  # angle above horizontal
    neckL = bodyR * rng.uniform(0.5, 2.2)
    headC = b1 + np.array([0, np.sin(neckA) * neckL + headR * 0.3,
                           np.cos(neckA) * neckL])
    parts.append((lambda n, a=b1, b=headC, r=headR * 0.45:
                  capsule_surf(a, b, r, n, rng), HEAD, neckL * headR))
    parts.append((lambda n, c=headC, r=headR: sphere_surf(c, r, n, rng),
                  HEAD, headR * headR * 8))
    if rng.random() < 0.7:      # muzzle forward
        mc = headC + np.array([0, -headR * 0.15, headR * rng.uniform(0.8, 1.3)])
        parts.append((lambda n, c=mc, r=headR * rng.uniform(0.3, 0.55):
                      sphere_surf(c, r, n, rng), HEAD, headR * headR * 3))
    if rng.random() < 0.7:      # ears up
        for sx in (+1, -1):
            ec = headC + np.array([sx * headR * 0.7, headR * rng.uniform(0.6, 1.1), 0])
            parts.append((lambda n, c=ec, r=headR * rng.uniform(0.2, 0.45):
                          sphere_surf(c, r, n, rng), HEAD, headR * headR * 2))
    if rng.random() < 0.8:      # tail at -Z → TORSO (no tail label; matches rig prior)
        tailA = rng.uniform(-0.5, 1.0); tailL = bodyL * rng.uniform(0.25, 0.7)
        t1 = b0 + np.array([0, np.sin(tailA) * tailL, -np.cos(tailA) * tailL])
        parts.append((lambda n, a=b0, b=t1, r=bodyR * rng.uniform(0.1, 0.3):
                      capsule_surf(a, b, r, n, rng), TORSO, tailL * bodyR))
    for sx, l in ((+1, LLEG), (-1, RLEG)):
        for zf in (+1, -1):
            hip = np.array([sx * bodyR * 0.8, y0 - bodyR * 0.3, zf * bodyL * 0.38])
            foot = hip + np.array([0, -(legL + bodyR * 0.7 - bodyR * 0.3), 0])
            parts.append((lambda n, a=hip, b=foot, r=legR:
                          capsule_surf(a, b, r, n, rng), l, legL * legR * 2))
            if rng.random() < 0.5:  # hoof/paw, slight +Z
                parts.append((lambda n, c=foot + np.array([0, 0, legR * 0.5]),
                              h=np.array([legR, legR * 0.6, legR * 1.6]):
                              box_surf(c, h, n, rng), l, legR * legR * 3))
    return parts


def make_biped_tail(rng):
    """Dino-style: horizontal-ish body, long tail (TORSO), two legs, small arms."""
    parts = []
    bodyL = rng.uniform(0.4, 0.8); bodyR = rng.uniform(0.14, 0.28)
    legL = rng.uniform(1.0, 2.0) * bodyR; legR = bodyR * rng.uniform(0.25, 0.5)
    y0 = legL + bodyR * 0.5
    tilt = rng.uniform(0.1, 0.5)
    b0 = np.array([0, y0 - np.sin(tilt) * bodyL / 2, -np.cos(tilt) * bodyL / 2])
    b1 = np.array([0, y0 + np.sin(tilt) * bodyL / 2, np.cos(tilt) * bodyL / 2])
    parts.append((lambda n, a=b0, b=b1, r=bodyR: capsule_surf(a, b, r, n, rng),
                  TORSO, bodyL * bodyR * 3))
    tailL = bodyL * rng.uniform(0.6, 1.3)
    t1 = b0 + np.array([0, rng.uniform(-0.1, 0.25) * tailL, -tailL])
    parts.append((lambda n, a=b0, b=t1, r=bodyR * rng.uniform(0.25, 0.5):
                  capsule_surf(a, b, r, n, rng), TORSO, tailL * bodyR * 1.5))
    headR = bodyR * rng.uniform(0.5, 0.9)
    neckL = bodyR * rng.uniform(0.8, 2.5); neckA = rng.uniform(0.5, 1.3)
    headC = b1 + np.array([0, np.sin(neckA) * neckL + headR * 0.3, np.cos(neckA) * neckL])
    parts.append((lambda n, a=b1, b=headC, r=headR * 0.45:
                  capsule_surf(a, b, r, n, rng), HEAD, neckL * headR))
    parts.append((lambda n, c=headC, r=headR,
                  s=np.array([0.9, 0.9, rng.uniform(1.0, 1.6)]):
                  sphere_surf(c, r, n, rng, s), HEAD, headR * headR * 8))
    if rng.random() < 0.5:      # tiny arms
        for sx, l in ((+1, LARM), (-1, RARM)):
            sh = b1 + np.array([sx * bodyR * 0.9, 0, 0])
            w = sh + np.array([sx * 0.1, -bodyR * rng.uniform(0.5, 1.2), bodyR * 0.6])
            parts.append((lambda n, a=sh, b=w, r=legR * 0.5:
                          capsule_surf(a, b, r, n, rng), l, bodyR * legR))
    for sx, l in ((+1, LLEG), (-1, RLEG)):
        hip = np.array([sx * bodyR * 0.85, y0 - bodyR * 0.2, rng.uniform(-0.1, 0.1)])
        foot = np.array([hip[0], 0.0, hip[2]])
        parts.append((lambda n, a=hip, b=foot, r=legR:
                      capsule_surf(a, b, r, n, rng), l, legL * legR * 2))
        footL = legR * rng.uniform(1.6, 2.6)
        parts.append((lambda n, c=foot + np.array([0, legR * 0.4, footL * 0.4]),
                      h=np.array([legR, legR * 0.5, footL]): box_surf(c, h, n, rng),
                      l, footL * legR * 3))
    return parts


def sample_body(rng, n_out=N_BASE):
    """Build one synthetic body: pick a plan, sample surfaces with randomised
    per-part density, augment, normalise. Returns (P [n,3] f32, L [n] i64)."""
    roll = rng.random()
    if roll < 0.62: parts = make_humanoid(rng)
    elif roll < 0.85: parts = make_quadruped(rng)
    else: parts = make_biped_tail(rng)

    # per-part counts ∝ area × random density (heads are often 30-50% of real
    # meshes' vertices — faces are dense — so heads get a wider density range)
    ws = np.array([w * (rng.uniform(0.8, 5.0) if l == HEAD else rng.uniform(0.5, 2.5))
                   for _, l, w in parts])
    ws /= ws.sum()
    counts = np.maximum(8, (ws * n_out * 1.5).astype(int))
    pts, lab = [], []
    for (fn, l, _), cnt in zip(parts, counts):
        pts.append(fn(int(cnt))); lab.append(np.full(int(cnt), l, np.int64))
    P = np.concatenate(pts).astype(np.float32); L = np.concatenate(lab)

    P = augment(P, rng)
    idx = rng.choice(len(P), n_out, replace=len(P) < n_out)
    return normalise(P[idx]), L[idx]


# --- augmentation / normalisation --------------------------------------------
def augment(P, rng):
    """Yaw (mostly near-canonical, sometimes full 360°), small tilt, anisotropic
    scale, jitter. Facing stays learnable: feet/muzzle cues survive yaw."""
    yaw = rng.uniform(-np.pi, np.pi) if rng.random() < 0.15 else rng.normal(0, 0.35)
    tilt = rng.uniform(-0.10, 0.10, size=2)
    cy, sy = np.cos(yaw), np.sin(yaw)
    Ry = np.array([[cy, 0, sy], [0, 1, 0], [-sy, 0, cy]])
    cx, sx = np.cos(tilt[0]), np.sin(tilt[0]); cz, sz = np.cos(tilt[1]), np.sin(tilt[1])
    Rx = np.array([[1, 0, 0], [0, cx, -sx], [0, sx, cx]])
    Rz = np.array([[cz, -sz, 0], [sz, cz, 0], [0, 0, 1]])
    out = P * rng.uniform(0.85, 1.2, size=3).astype(np.float32)   # anisotropic
    out = out @ (Ry @ Rx @ Rz).T.astype(np.float32)
    out += rng.normal(scale=0.004, size=out.shape).astype(np.float32)
    return out.astype(np.float32)


def mirror(P, L):
    """Reflect across the sagittal plane: x → -x, swap left/right labels."""
    P2 = P.copy(); P2[:, 0] = -P2[:, 0]
    swap = np.array([0, 1, 2, RARM, LARM, RLEG, LLEG])
    return P2, swap[L]


def normalise(P):
    c = 0.5 * (P.min(0) + P.max(0)); h = float(np.max(0.5 * (P.max(0) - P.min(0))))
    return ((P - c) / (h + 1e-9)).astype(np.float32)


def gen_dataset(samples, seed):
    rng = np.random.default_rng(seed)
    aP = np.zeros((samples, N_BASE, 3), np.float32)
    aL = np.zeros((samples, N_BASE), np.int64)
    for i in range(samples):
        P, L = sample_body(rng)
        if rng.random() < 0.5: P, L = mirror(P, L)
        aP[i] = P; aL[i] = L
    return aP, aL


# --- mined real meshes (rig-prior ground truth) ------------------------------
def canonicalise(P, L, fname=""):
    """Rotate a mined cloud into the canonical frame using its own labels:
    up = the legs' own principal axis (robust to tails skewing the torso
    centroid), left = right→left limb axis, forward = left×up, with a 180°
    yaw fix for animal clouds whose head lands backward. Then geometrically
    REASSIGN arm/leg sides (miner side detection is unreliable on some rigs).
    Returns (P, L, ok, msg)."""
    cent = {p: P[L == p].mean(0) for p in range(1, 7) if (L == p).sum() >= 8}
    legs = [cent[p] for p in (LLEG, RLEG) if p in cent]
    if not legs:
        return P, L, False, "no leg labels — cannot infer up axis"
    legs_c = np.mean(legs, axis=0)
    body_c = cent.get(TORSO, cent.get(HEAD))
    if body_c is None:
        return P, L, False, "no torso/head labels"
    # Two "up" candidates, scored below by how many leg points land below the
    # body: (A) torso−legs centroid vector — right for humanoids and compact
    # quadrupeds; (B) per-limb leg PCA — right when a big tail drags the torso
    # centroid backward (dinos). Each limb cluster is isolated by splitting a
    # side's leg points along their spread axis so front+hind legs of a
    # quadruped don't lump into one cluster whose PCA is the body axis.
    candidates = [body_c - legs_c]
    pca_ups = []
    for p in (LLEG, RLEG):
        if p not in cent or (L == p).sum() < 40: continue
        X = P[L == p]
        spread = X.max(0) - X.min(0)
        ax = int(np.argmax(spread))
        halves = [X[X[:, ax] <= np.median(X[:, ax])], X[X[:, ax] > np.median(X[:, ax])]]
        for h in halves:
            if len(h) < 20: continue
            Xc = h - h.mean(0)
            v = np.linalg.eigh(Xc.T @ Xc)[1][:, -1]
            s = float((body_c - h.mean(0)) @ v)
            if abs(s) > 1e-9:
                pca_ups.append(v * np.sign(s))
    if pca_ups:
        candidates.append(np.mean(pca_ups, axis=0))

    def build_frame(up):
        if np.linalg.norm(up) < 1e-6: return None
        up = up / np.linalg.norm(up)
        if LARM in cent and RARM in cent:
            left = cent[LARM] - cent[RARM]
        elif LLEG in cent and RLEG in cent:
            left = cent[LLEG] - cent[RLEG]
        else:
            return None
        left = left - (left @ up) * up
        if np.linalg.norm(left) < 1e-6: return None
        left = left / np.linalg.norm(left)
        R = np.stack([left, up, np.cross(left, up)])   # rows = canonical axes
        Pc = (P @ R.T).astype(np.float32)
        # Head anatomically defines "forward". If it protrudes BACKWARD (-Z)
        # more than it rises above the torso (animal-like silhouette), the side
        # labels driving `left` were majority-flipped, or the rig faces
        # backward — a 180° yaw fixes forward AND the sides in one go.
        # (Humanoid heads sit ABOVE the torso with |z| tiny, so they never
        # trigger; their facing is untestable from centroids and their sides
        # come from the majority-correct arm labels.)
        if HEAD in cent:
            hc = Pc[L == HEAD].mean(0)
            tc = Pc[L == TORSO].mean(0) if TORSO in cent else hc
            if hc[2] < -0.05 and 0.8 * abs(hc[2]) > max(0.0, hc[1] - tc[1]):
                Pc[:, 0] = -Pc[:, 0]; Pc[:, 2] = -Pc[:, 2]
        legm = (L == LLEG) | (L == RLEG)
        score = float((Pc[legm, 1] < np.median(Pc[:, 1])).mean())
        if HEAD in cent:
            hc = Pc[L == HEAD].mean(0)
            ly = Pc[legm, 1].mean()
            score += 0.5 * float(hc[1] > ly or hc[2] > 0.05)
        return Pc, score

    frames = [f for f in (build_frame(u) for u in candidates) if f is not None]
    if not frames:
        return P, L, False, "no bilateral limb pair / degenerate axes"
    Pc, score = max(frames, key=lambda t: t[1])
    # require BOTH a coherent head (above legs or forward) and most leg points
    # below the body — incoherent clouds would only inject label noise
    if score < 0.95:
        return P, L, False, f"incoherent after canonicalisation (score {score:.2f})"

    # geometric side reassignment (bind poses are bilaterally lateralised)
    Lc = L.copy()
    for a, b in ((LARM, RARM), (LLEG, RLEG)):
        m = (Lc == a) | (Lc == b)
        if m.sum() == 0: continue
        xm = Pc[m, 0] - np.median(Pc[:, 0])
        Lc[m] = np.where(xm >= 0, a, b)
    return normalise(Pc), Lc, True, ""


def load_real_data(paths, aug, seed, exclude=(), only=()):
    rng = np.random.default_rng(seed + 777)
    files = []
    for pth in paths:
        files += sorted(glob.glob(os.path.join(pth, "*.json"))) if os.path.isdir(pth) else [pth]
    files = [f for f in files if not any(e in os.path.basename(f) for e in exclude)]
    if only:
        files = [f for f in files if any(o in os.path.basename(f) for o in only)]
    Ps, Ls = [], []
    for f in files:
        d = json.load(open(f))
        if d.get("schema") != "qtmesh-meshseg-training-v1":
            print(f"  skip {f}: unexpected schema {d.get('schema')!r}"); continue
        P = np.asarray(d["points"], np.float32).reshape(-1, 3)
        L = np.asarray(d["labels"], np.int64)
        if len(P) != len(L) or len(P) == 0:
            print(f"  skip {f}: points/labels mismatch"); continue
        P, L, ok, msg = canonicalise(P, L, f)
        if not ok:
            print(f"  skip {os.path.basename(f)}: {msg}"); continue
        for k in range(1 + aug):
            idx = rng.choice(len(P), N_BASE, replace=len(P) < N_BASE)
            pp, ll = P[idx], L[idx]
            if k > 0:
                pp = normalise(augment(pp, rng))
            if k % 2 == 1:
                pp, ll = mirror(pp, ll)
            Ps.append(pp); Ls.append(ll)
    print(f"loaded {len(files)} real mesh file(s) -> {len(Ps)} samples (incl. {aug}x aug)")
    if not Ps:
        return np.zeros((0, N_BASE, 3), np.float32), np.zeros((0, N_BASE), np.int64)
    return np.stack(Ps), np.stack(Ls)


def check_real(paths):
    """--check-real: print canonicalisation sanity for every mined file."""
    files = []
    for pth in paths:
        files += sorted(glob.glob(os.path.join(pth, "*.json"))) if os.path.isdir(pth) else [pth]
    bad = 0
    for f in files:
        d = json.load(open(f))
        P = np.asarray(d["points"], np.float32).reshape(-1, 3)
        L = np.asarray(d["labels"], np.int64)
        Pc, Lc, ok, msg = canonicalise(P, L, f)
        name = os.path.basename(f)[:44]
        if not ok:
            print(f"FAIL {name:44s} {msg}"); bad += 1; continue
        hy = Pc[Lc == HEAD, 1].mean() if (Lc == HEAD).any() else float("nan")
        hz = Pc[Lc == HEAD, 2].mean() if (Lc == HEAD).any() else float("nan")
        ly = Pc[(Lc == LLEG) | (Lc == RLEG), 1].mean()
        lx = Pc[Lc == LARM, 0].mean() if (Lc == LARM).any() else float("nan")
        rx = Pc[Lc == RARM, 0].mean() if (Lc == RARM).any() else float("nan")
        # head must be above the legs (humanoid) OR forward at +Z (animal)
        good = (np.isnan(hy) or hy > ly or hz > 0.05) \
            and (np.isnan(lx) or lx > 0) and (np.isnan(rx) or rx < 0)
        if not good: bad += 1
        print(f"{'ok  ' if good else 'BAD '}{name:44s} head_y={hy:+.2f} head_z={hz:+.2f} "
              f"legs_y={ly:+.2f} larm_x={lx:+.2f} rarm_x={rx:+.2f}")
    print(f"\n{len(files) - bad}/{len(files)} canonicalised cleanly")


# --- training -----------------------------------------------------------------
def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--samples", type=int, default=4000)
    ap.add_argument("--epochs", type=int, default=40, help="phase-1 epochs @2048 pts")
    ap.add_argument("--epochs2", type=int, default=6, help="phase-2 epochs @4096 pts")
    ap.add_argument("--batch", type=int, default=8)
    ap.add_argument("--out", default="meshseg.onnx")
    ap.add_argument("--seed", type=int, default=0)
    ap.add_argument("--real-data", nargs="*", default=[],
                    help="dirs/files of `qtmesh segment --dump-training-data` JSON "
                         "samples (rig-prior ground truth) to MIX with synthetic data")
    ap.add_argument("--real-aug", type=int, default=23,
                    help="augmented copies per real mesh (yaw/tilt/scale/mirror)")
    ap.add_argument("--val-real", nargs="*", default=[],
                    help="basename substrings of mined files HELD OUT of training "
                         "and used as the real validation set")
    ap.add_argument("--check-real", action="store_true",
                    help="only sanity-check canonicalisation of --real-data, then exit")
    a = ap.parse_args()

    if a.check_real:
        check_real(a.real_data); return

    import torch
    import torch.nn as nn

    print("generating synthetic data…")
    P, L = gen_dataset(a.samples, a.seed)
    if a.real_data:
        rP, rL = load_real_data(a.real_data, a.real_aug, a.seed, exclude=a.val_real)
        if len(rP):
            P = np.concatenate([P, rP]); L = np.concatenate([L, rL])
            sh = np.random.default_rng(a.seed + 1).permutation(len(P))
            P = P[sh]; L = L[sh]
            print(f"combined dataset: {len(P)} samples "
                  f"({a.samples} synthetic + {len(rP)} real)")
    # real validation set: held-out mined files, no augmentation
    vP = vL = None
    if a.real_data and a.val_real:
        vP, vL = load_real_data(a.real_data, 0, a.seed, only=a.val_real)
        if not len(vP): vP = vL = None

    P = torch.tensor(P); L = torch.tensor(L)
    n = P.shape[0]
    nval = max(1, n // 20)
    dev = "mps" if torch.backends.mps.is_available() else \
          ("cuda" if torch.cuda.is_available() else "cpu")
    print(f"data n={n} stored_points={N_BASE} dev={dev}")

    # class weights: unknown masked out, others inverse-sqrt frequency
    freq = np.bincount(L.numpy().ravel(), minlength=C).astype(np.float64)
    w = np.zeros(C); nz = freq > 0
    w[nz] = 1.0 / np.sqrt(freq[nz]); w[0] = 0.0
    w = w / w[nz & (np.arange(C) > 0)].mean()
    print("class weights:", np.round(w, 2))
    cw = torch.tensor(w, dtype=torch.float32, device=dev)

    class PointSeg(nn.Module):
        # PointNet++-style: per-point MLP + TWO kNN local-aggregation blocks
        # (both reuse one in-graph cdist+topk neighbourhood — ONNX-exportable)
        # + a global max-pooled feature.
        def __init__(s, d=128, k=12):
            super().__init__()
            s.k = k
            s.mlp1 = nn.Sequential(nn.Linear(3, 64), nn.GELU(), nn.Linear(64, d), nn.GELU())
            s.loc1 = nn.Sequential(nn.Linear(2 * d, d), nn.GELU(), nn.Linear(d, d), nn.GELU())
            s.loc2 = nn.Sequential(nn.Linear(2 * d, d), nn.GELU(), nn.Linear(d, d), nn.GELU())
            s.mlp2 = nn.Sequential(nn.Linear(d, d), nn.GELU(), nn.Linear(d, d), nn.GELU())
            s.head = nn.Sequential(nn.Linear(3 * d, d), nn.GELU(), nn.Linear(d, C))

        def _agg(s, f, nbr):
            B, N, dimf = f.shape
            kk = nbr.shape[-1]
            idx = nbr.reshape(B, N * kk, 1).expand(-1, -1, dimf)
            gathered = torch.gather(f, 1, idx).reshape(B, N, kk, dimf)
            rel = gathered - f.unsqueeze(2)
            return torch.cat([f, rel.max(dim=2).values], dim=-1)

        def forward(s, pts):                       # pts: [B,N,3]
            f = s.mlp1(pts)
            d2 = torch.cdist(pts, pts)
            kk = min(s.k, pts.shape[1])
            nbr = d2.topk(kk, dim=-1, largest=False).indices
            l1 = s.loc1(s._agg(f, nbr))
            l2 = s.loc2(s._agg(l1, nbr))
            g = s.mlp2(l2)
            glob = g.max(dim=1, keepdim=True).values.expand(-1, pts.shape[1], -1)
            return s.head(torch.cat([f, l2, glob], dim=-1))

    net = PointSeg().to(dev)
    lossf = nn.CrossEntropyLoss(weight=cw, ignore_index=0)

    def run_val(np_pts):
        net.eval()
        correct = 0; total = 0
        with torch.no_grad():
            for b in range(0, nval, 4):
                pv = P[b:b + 4, :np_pts].to(dev); lv = L[b:b + 4, :np_pts].to(dev)
                m = lv > 0
                correct += ((net(pv).argmax(-1) == lv) & m).sum().item()
                total += m.sum().item()
        rp = ""
        if vP is not None:
            vc = 0; vt = 0
            with torch.no_grad():
                for b in range(0, len(vP), 4):
                    pv = torch.tensor(vP[b:b + 4]).to(dev)
                    lv = torch.tensor(vL[b:b + 4]).to(dev)
                    m = lv > 0
                    vc += ((net(pv).argmax(-1) == lv) & m).sum().item()
                    vt += m.sum().item()
            rp = f" real_val_acc={vc / max(1, vt):.4f}"
        return f"val_acc={correct / max(1, total):.4f}{rp}"

    def train_phase(epochs, np_pts, lr, tag):
        opt = torch.optim.AdamW(net.parameters(), lr=lr, weight_decay=1e-4)
        sch = torch.optim.lr_scheduler.CosineAnnealingLR(opt, epochs)
        bs = a.batch if np_pts <= 2048 else max(2, a.batch // 2)
        g = torch.Generator().manual_seed(a.seed)
        for ep in range(epochs):
            net.train()
            perm = torch.randperm(n - nval, generator=g) + nval
            last = 0.0
            for b in range(0, perm.numel(), bs):
                bi = perm[b:b + bs]
                pts = P[bi]; lab = L[bi]
                if np_pts < N_BASE:   # random per-sample subset = free augmentation
                    sub = torch.randint(0, N_BASE, (len(bi), np_pts), generator=g)
                    pts = torch.gather(pts, 1, sub.unsqueeze(-1).expand(-1, -1, 3))
                    lab = torch.gather(lab, 1, sub)
                logits = net(pts.to(dev))
                loss = lossf(logits.reshape(-1, C), lab.to(dev).reshape(-1))
                opt.zero_grad(); loss.backward(); opt.step()
                last = loss.item()
            sch.step()
            if ep % 2 == 0 or ep == epochs - 1:
                print(f"{tag} ep{ep:3d} loss{last:.4f} {run_val(np_pts)}", flush=True)

    train_phase(a.epochs, 2048, 2e-3, "p1")
    train_phase(a.epochs2, N_BASE, 3e-4, "p2")   # match inference point count

    net.eval().cpu()
    torch.onnx.export(
        net, (torch.zeros(1, N_BASE, 3),), a.out,
        input_names=["points"], output_names=["logits"],
        dynamic_axes={"points": {1: "N"}, "logits": {1: "N"}},
        opset_version=17, dynamo=False)
    print("wrote", a.out)


if __name__ == "__main__":
    main()
