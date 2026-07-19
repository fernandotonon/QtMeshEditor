#!/usr/bin/env python3
# ruff: noqa: E702, E741
#   This is a compact, offline, NOT-shipped dev tool: a few `;`-joined helper
#   one-liners (E702) and the `l` part-label loop var (E741) are intentional for
#   density. The app never runs this file.
"""Train + export the mesh part-segmentation models to ONNX (#410, #788, #818 B2).

ONE-TIME, OFFLINE developer tool — NOT shipped with the app, and the app never
runs Python. The app runs the resulting .onnx files in C++ via ONNX Runtime
(src/MeshSegmenter.cpp), downloading them on first use to
AppData/ai_models/segment/.

WHAT IT PRODUCES (select with --category; default `body` = the original model)
  Per-category segmenters with the contract MeshSegmenter::predict() expects:
    input  "points" float32 [1, N, 3]   (point cloud, centred unit box)
    output "logits" float32 [1, N, C]   (per-point class logits)
  Channel order MUST match the per-category channel→Part maps in
  src/MeshSegmenter.cpp (kCategoryChannelMaps):
    body       (meshseg.onnx, C=7):  0 unknown, 1 head, 2 torso, 3 left_arm,
                                     4 right_arm, 5 left_leg, 6 right_leg
    vegetation (meshseg_vegetation.onnx, C=6): 0 unknown, 1 trunk, 2 branch,
                                     3 foliage, 4 root, 5 flower
    vehicle    (meshseg_vehicle.onnx, C=6):    0 unknown, 1 body, 2 wheel,
                                     3 window, 4 wing, 5 rotor
    building   (meshseg_building.onnx, C=7):   0 unknown, 1 wall, 2 roof,
                                     3 window, 4 door, 5 chimney, 6 foundation
  Plus `--category classifier` (meshseg_category.onnx): a tiny point-cloud
  CATEGORY classifier, input "points" [1, N, 3] → output "logits" [1, 4]
  (0 body, 1 vegetation, 2 vehicle, 3 building) — the Auto dispatcher.
  Frame convention (matches the app's inference path): +Y up, character/vehicle
  facing +Z, LEFT limbs at +X. MeshSegmenter::predict() remaps the user's
  --up-axis onto Y; facing is learned (feet/muzzle/car-nose point +Z in
  training, with yaw augmentation for robustness).

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
    # per-category segmenters (#818 B2, synthetic-only):
    python export-meshseg-onnx.py --category vegetation --samples 3000
    python export-meshseg-onnx.py --category vehicle --samples 3000
    python export-meshseg-onnx.py --category building --samples 3000
    # the Auto-dispatch category classifier (real data mixed in as `body`):
    python export-meshseg-onnx.py --category classifier --samples 6000 \
        --real-data ./mined/
"""
import argparse
import glob
import json
import os

import numpy as np

HEAD, TORSO, LARM, RARM, LLEG, RLEG = 1, 2, 3, 4, 5, 6
C = 7  # body Part channel count (meshseg.onnx wire contract)
N_BASE = 4096  # stored points per sample == the app's inference sample size

# per-category LOCAL channel indices (0 = unknown everywhere); the C++ side
# maps these to the global MeshSegmenter::Part enum via kCategoryChannelMaps
TRUNK, BRANCH, FOLIAGE, ROOT, FLOWER = 1, 2, 3, 4, 5                # vegetation
VBODY, WHEEL, WINDOW, WING, ROTOR = 1, 2, 3, 4, 5                   # vehicle
WALL, ROOF, BWINDOW, DOOR, CHIMNEY, FOUNDATION = 1, 2, 3, 4, 5, 6   # building

CATEGORIES = {
    #  name       C  out-file suffix
    "body":       (7, "meshseg.onnx"),
    "vegetation": (6, "meshseg_vegetation.onnx"),
    "vehicle":    (6, "meshseg_vehicle.onnx"),
    "building":   (7, "meshseg_building.onnx"),
}
CLASSIFIER_CLASSES = ["body", "vegetation", "vehicle", "building"]


# --- surface samplers --------------------------------------------------------
def _unit_dirs(n, rng):
    v = rng.normal(size=(n, 3)); v /= np.linalg.norm(v, axis=1, keepdims=True) + 1e-9
    return v


def sphere_surf(c, r, n, rng, squash=None):
    """Points on an ellipsoid surface (r scalar radius, squash per-axis scale)."""
    p = _unit_dirs(n, rng) * r
    if squash is not None: p = p * squash
    return c + p


def ball_vol(c, r, n, rng, squash=None):
    """Points filling an ellipsoid VOLUME (not just the surface). A stylized
    tree's canopy is a dense cloud of leaf cards filling a volume, not a hollow
    shell — training only on shells taught the model that a solid low mass must
    be trunk (the oak-canopy-labelled-trunk bug)."""
    d = _unit_dirs(n, rng) * (rng.random((n, 1)) ** (1.0 / 3.0)) * r
    if squash is not None: d = d * squash
    return c + d


def capsule_surf(p0, p1, r, n, rng, cap0=True, cap1=True):
    """Points on a capsule surface (cylinder side + spherical caps).

    cap0/cap1 disable sampling the p0/p1 end cap. Use for ATTACHMENT ends
    (an arm's shoulder end, a leg's hip end, a neck's torso end): a real
    character has NO surface at the limb-torso junction — the limb fuses
    into the body. Sampling that cap plants limb-labelled points exactly on
    the torso boundary and teaches the model to over-claim it (the #788
    retrain regression: with the exterior-cap fix those points all landed
    on the junction-facing hemisphere and torso recall fell 0.80 → 0.50-0.68)."""
    p0 = np.asarray(p0, float); p1 = np.asarray(p1, float)
    axis = p1 - p0; L = np.linalg.norm(axis) + 1e-9; u = axis / L
    ncaps = int(cap0) + int(cap1)
    # area split: side 2πrL vs caps 2πr² each
    side_frac = L / (L + ncaps * r) if ncaps else 1.0
    ns = int(n * side_frac); nc = n - ns
    # side: random t along axis, random dir ⊥ axis
    t = rng.random((ns, 1))
    d = _unit_dirs(ns, rng); d -= (d @ u)[:, None] * u
    d /= np.linalg.norm(d, axis=1, keepdims=True) + 1e-9
    side = p0 + t * axis + d * r
    if nc <= 0:
        return side
    caps_dir = _unit_dirs(nc, rng)
    if cap0 and cap1:
        which = rng.random(nc) < 0.5
    else:
        which = np.full(nc, bool(cap0))   # all points on the one enabled cap
    # keep caps on the EXTERIOR: reflect directions pointing into the shaft
    # onto the outward hemisphere (p0 cap faces -u, p1 cap faces +u) — a full
    # sphere would put half the cap points inside the cylinder, off-surface
    along = caps_dir @ u
    flip = (which & (along > 0)) | (~which & (along < 0))
    caps_dir[flip] -= 2 * along[flip, None] * u
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


def quad_surf(origin, e1, e2, n, rng):
    """Points on a parallelogram patch origin + u·e1 + v·e2, u,v ∈ [0,1]."""
    uv = rng.random((n, 2))
    return np.asarray(origin, float) + uv[:, :1] * np.asarray(e1, float) \
        + uv[:, 1:] * np.asarray(e2, float)


def tri_surf(a, b, c, n, rng):
    """Points on a triangle (uniform barycentric)."""
    uv = rng.random((n, 2))
    flip = uv.sum(1) > 1
    uv[flip] = 1 - uv[flip]
    a = np.asarray(a, float)
    return a + uv[:, :1] * (np.asarray(b, float) - a) \
        + uv[:, 1:] * (np.asarray(c, float) - a)


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

    # neck + head (both HEAD — bone-name convention maps neck→head). Both neck
    # ends are junctions (torso below, head sphere above) — no caps.
    neckL = torsoH * rng.uniform(0.05, 0.25)
    neckTop = np.array([0, pel + torsoH + neckL, 0])
    parts.append((lambda n, a=np.array([0, pel + torsoH, 0]), b=neckTop,
                  r=headR * rng.uniform(0.25, 0.5):
                  capsule_surf(a, b, r, n, rng, cap0=False, cap1=False),
                  HEAD, neckL * headR * 2))
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
        # No cap at the shoulder end (segment 0's p0) — arm-torso junction.
        for si, (a, b) in enumerate(segs):
            parts.append((lambda n, a=a, b=b, r=armR, c0=si > 0:
                          capsule_surf(a, b, r, n, rng, cap0=c0),
                          l, armLen * armR))
        handR = armR * rng.uniform(1.1, 1.7)
        parts.append((lambda n, c=wrist, r=handR: sphere_surf(c, r, n, rng),
                      l, handR * handR * 5))

    stance = rng.uniform(0.0, 0.35)
    for sx, l in ((+1, LLEG), (-1, RLEG)):
        hip = np.array([sx * torsoW * rng.uniform(0.4, 0.75), pel, 0])
        ankle = hip + np.array([sx * stance * legL, -legL, rng.uniform(-0.05, 0.05)])
        # No cap at the hip end — leg-torso junction.
        parts.append((lambda n, a=hip, b=ankle, r=legR:
                      capsule_surf(a, b, r, n, rng, cap0=False),
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
                  capsule_surf(a, b, r, n, rng, cap0=False, cap1=False),
                  HEAD, neckL * headR))
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
                          capsule_surf(a, b, r, n, rng, cap0=False),
                          l, legL * legR * 2))
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
                  capsule_surf(a, b, r, n, rng, cap0=False, cap1=False),
                  HEAD, neckL * headR))
    parts.append((lambda n, c=headC, r=headR,
                  s=np.array([0.9, 0.9, rng.uniform(1.0, 1.6)]):
                  sphere_surf(c, r, n, rng, s), HEAD, headR * headR * 8))
    if rng.random() < 0.5:      # tiny arms (no cap at the body junction)
        for sx, l in ((+1, LARM), (-1, RARM)):
            sh = b1 + np.array([sx * bodyR * 0.9, 0, 0])
            w = sh + np.array([sx * 0.1, -bodyR * rng.uniform(0.5, 1.2), bodyR * 0.6])
            parts.append((lambda n, a=sh, b=w, r=legR * 0.5:
                          capsule_surf(a, b, r, n, rng, cap0=False), l, bodyR * legR))
    for sx, l in ((+1, LLEG), (-1, RLEG)):
        hip = np.array([sx * bodyR * 0.85, y0 - bodyR * 0.2, rng.uniform(-0.1, 0.1)])
        foot = np.array([hip[0], 0.0, hip[2]])
        parts.append((lambda n, a=hip, b=foot, r=legR:
                      capsule_surf(a, b, r, n, rng, cap0=False), l, legL * legR * 2))
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


# --- synthetic vegetation (trunk/branch/foliage/root/flower) -----------------
def make_tree(rng):
    """Surface-sampled tree/plant. Same (sampler, label, weight) contract as the
    body plans; labels are the vegetation LOCAL channels."""
    parts = []
    kind = rng.choice(['broadleaf', 'oak', 'pine', 'palm', 'dead', 'bush'],
                      p=[0.30, 0.22, 0.20, 0.12, 0.08, 0.08])
    trunkH = rng.uniform(0.5, 1.5)
    trunkR = trunkH * rng.uniform(0.04, 0.12)
    if kind == 'bush':
        trunkH *= rng.uniform(0.15, 0.4)
    # 'oak': big-canopy / short-trunk broadleaf — a huge canopy on a stubby
    # trunk, with foliage DROOPING down around/below the trunk top (real oaks,
    # willows, stylized game trees). This is the regime the oak-canopy bug lives
    # in: the canopy dwarfs the trunk and its lower edge dips into trunk height.
    canopyScale = 1.0        # canopy radius multiplier vs trunkH
    droop = 0.0              # how far the canopy center sits BELOW the trunk top
    if kind == 'oak':
        trunkH *= rng.uniform(0.45, 0.9)       # stubby
        trunkR = trunkH * rng.uniform(0.10, 0.22)
        canopyScale = rng.uniform(1.1, 2.2)    # canopy much bigger than trunk
        droop = rng.uniform(0.15, 0.55)        # canopy descends around the trunk
    lean = rng.uniform(-0.12, 0.12, size=2)
    top = np.array([lean[0] * trunkH, trunkH, lean[1] * trunkH])
    parts.append((lambda n, b=top, r=trunkR: capsule_surf([0, 0, 0], b, r, n, rng),
                  TRUNK, trunkH * trunkR * 3))
    # Trunk base FLARE / buttress — most real trunks widen at the bottom. This is
    # TRUNK, not root: without it the model learned "wide low mass = root" and
    # grabbed the trunk flare (user report). A short, wide, low cone at the base.
    if rng.random() < 0.7:
        flareR = trunkR * rng.uniform(1.6, 3.5)
        flareH = trunkH * rng.uniform(0.05, 0.18)
        parts.append((lambda n, top=np.array([0.0, flareH, 0.0]), r=flareR:
                      capsule_surf([0, 0, 0], top, r, n, rng),
                      TRUNK, flareR * flareH * 4))

    def blob(c, r, label, w, squash=None, solid=False):
        fn = ball_vol if solid else sphere_surf
        parts.append((lambda n, c=c, r=r, s=squash: fn(c, r, n, rng, s), label, w))

    if kind in ('broadleaf', 'oak', 'dead', 'bush'):
        # More primary branches, and each spawns a couple of thinner SUB-branches
        # that thread UP INTO the canopy — real trees (esp. the user's oak) show
        # a visible branch skeleton inside the leaves. Under-representing branches
        # let trunk/foliage over-claim them (branch recall regression); branches
        # get a heavier weight + a density boost in sampling so they hold ground.
        nbr = rng.integers(4, 11 if kind == 'oak' else 9)
        tips = []
        for _ in range(nbr):
            az = rng.uniform(0, 2 * np.pi)
            # oak branches spread more horizontally (lower elevation) so the
            # canopy sits wide and low rather than piled on top
            elev = rng.uniform(-0.1, 0.6) if kind == 'oak' else rng.uniform(0.3, 1.1)
            bl = trunkH * rng.uniform(0.25, 0.7)
            base = top * rng.uniform(0.5, 0.95)
            tip = base + bl * np.array([np.cos(az) * np.cos(elev), np.sin(elev),
                                        np.sin(az) * np.cos(elev)])
            br = trunkR * rng.uniform(0.35, 0.65)
            parts.append((lambda n, a=base, b=tip, r=br:
                          capsule_surf(a, b, r, n, rng), BRANCH, bl * trunkR * 3))
            tips.append(tip)
            # sub-branches continuing from the tip deeper into the canopy
            for _ in range(rng.integers(0, 3)):
                d = _unit_dirs(1, rng)[0]; d[1] = abs(d[1]) * rng.uniform(0.3, 1.0)
                sl = bl * rng.uniform(0.3, 0.7)
                stip = tip + d * sl
                parts.append((lambda n, a=tip, b=stip, r=br * rng.uniform(0.5, 0.8):
                              capsule_surf(a, b, r, n, rng), BRANCH, sl * trunkR * 3))
        if kind != 'dead':
            # A meaningful share of canopies are SOLID-volume (dense leaf-card
            # clouds), not thin shells — the training-vs-real domain gap.
            solid = rng.random() < 0.5
            if kind == 'oak' or rng.random() < 0.5 or kind == 'bush':  # one big canopy
                cr = trunkH * canopyScale * rng.uniform(0.45, 0.85)
                # center can sit BELOW the trunk top (droop) so the canopy's
                # lower half overlaps trunk height — the oak silhouette
                cc = top + np.array([0, cr * rng.uniform(0.2, 0.7) - droop * trunkH, 0])
                blob(cc, cr, FOLIAGE, cr * cr * 10,
                     np.array([rng.uniform(0.85, 1.3), rng.uniform(0.6, 1.15),
                               rng.uniform(0.85, 1.3)]), solid=solid)
                # oak: a second, lower skirt of foliage that wraps the trunk
                if kind == 'oak' and rng.random() < 0.7:
                    sr = cr * rng.uniform(0.6, 0.95)
                    sc = np.array([top[0], trunkH * rng.uniform(0.35, 0.75), top[2]])
                    blob(sc, sr, FOLIAGE, sr * sr * 8,
                         np.array([rng.uniform(1.0, 1.5), rng.uniform(0.45, 0.8),
                                   rng.uniform(1.0, 1.5)]), solid=solid)
            else:                                        # per-tip blobs
                for tip in tips:
                    br = trunkH * rng.uniform(0.15, 0.35)
                    blob(tip, br, FOLIAGE, br * br * 8, solid=solid)
            if rng.random() < 0.25:                      # flowers / fruit
                for _ in range(rng.integers(2, 8)):
                    d = _unit_dirs(1, rng)[0]
                    fc = top + np.array([0, trunkH * 0.4, 0]) + d * trunkH * 0.45
                    blob(fc, trunkH * rng.uniform(0.02, 0.06), FLOWER, trunkH * 0.05)
    elif kind == 'pine':
        layers = rng.integers(3, 7)
        baseR = trunkH * rng.uniform(0.3, 0.55)
        for i in range(layers):
            t = (i + 1) / (layers + 1)
            lr = baseR * (1 - 0.75 * t)
            lc = np.array([0, trunkH * (0.35 + 0.75 * t), 0])
            blob(lc, lr, FOLIAGE, lr * lr * 6,
                 np.array([1, rng.uniform(0.25, 0.5), 1]))
    elif kind == 'palm':
        nfr = rng.integers(4, 9)
        for _ in range(nfr):
            az = rng.uniform(0, 2 * np.pi)
            fl = trunkH * rng.uniform(0.3, 0.55)
            fc = top + fl * 0.6 * np.array([np.cos(az), rng.uniform(-0.1, 0.35),
                                            np.sin(az)])
            sq = np.array([abs(np.cos(az)) * 2.2 + 0.3, 0.18,
                           abs(np.sin(az)) * 2.2 + 0.3])
            blob(fc, fl * 0.45, FOLIAGE, fl * fl * 2, sq)
        if rng.random() < 0.3:                           # coconuts
            for _ in range(rng.integers(2, 5)):
                d = _unit_dirs(1, rng)[0] * trunkR * 2
                blob(top + d, trunkR * rng.uniform(0.5, 1.0), FLOWER, trunkR)
    # Surface roots — only ~25% of trees (most real tree meshes model NO roots;
    # they're usually underground). When present they are a SMALL, GROUND-HUGGING
    # flare: thick where they meet the trunk (buttress-like, NOT twig-thin) and
    # spreading LOW and outward, never rising into trunk/branch height. Root
    # points stay in the bottom ~8% of the tree so the model learns "root = the
    # little bit right at the base", not "any thin low structure".
    if rng.random() < 0.25 and kind != 'bush':
        rootTopH = trunkH * rng.uniform(0.02, 0.08)      # ceiling: very low
        for _ in range(rng.integers(2, 5)):
            az = rng.uniform(0, 2 * np.pi)
            rl = trunkH * rng.uniform(0.06, 0.16)
            # spread outward and DOWN, ending at/below ground
            tip = np.array([np.cos(az) * rl, -trunkH * rng.uniform(0.0, 0.05),
                            np.sin(az) * rl])
            base = np.array([np.cos(az) * trunkR * 0.6, rootTopH,
                             np.sin(az) * trunkR * 0.6])
            parts.append((lambda n, a=base, b=tip,
                          r=trunkR * rng.uniform(0.5, 1.0):     # THICK, buttress-like
                          capsule_surf(a, b, r, n, rng, cap0=False),
                          ROOT, rl * trunkR * 2))
    return parts


# --- synthetic vehicles (body/wheel/window/wing/rotor), nose at +Z -----------
def make_vehicle(rng):
    parts = []
    kind = rng.choice(['car', 'truck', 'plane', 'heli'], p=[0.40, 0.15, 0.28, 0.17])

    def boxp(c, half, label, w):
        parts.append((lambda n, c=np.asarray(c, float), h=np.asarray(half, float):
                      box_surf(c, h, n, rng), label, w))

    def blob(c, r, label, w, squash=None):
        parts.append((lambda n, c=np.asarray(c, float), r=r, s=squash:
                      sphere_surf(c, r, n, rng, s), label, w))

    if kind in ('car', 'truck'):
        L = rng.uniform(0.8, 1.4); W = L * rng.uniform(0.35, 0.55)
        wheelR = L * rng.uniform(0.09, 0.16)
        bodyH = L * rng.uniform(0.12, 0.22)
        y0 = wheelR * rng.uniform(0.7, 1.1)
        boxp([0, y0 + bodyH / 2, 0], [W / 2, bodyH / 2, L / 2], VBODY, L * W * 2)
        cabL = L * rng.uniform(0.35, 0.55); cabH = bodyH * rng.uniform(0.7, 1.2)
        cabZ = (L / 2 - cabL / 2) * (rng.uniform(0.3, 0.9) if kind == 'car' else 0.95)
        cabY = y0 + bodyH + cabH / 2
        boxp([0, cabY, cabZ if kind == 'car' else L / 2 - cabL / 2],
             [W / 2 * 0.92, cabH / 2, cabL / 2], VBODY, cabL * W)
        # windows: thin proud panes on the cabin's four faces
        zc = cabZ if kind == 'car' else L / 2 - cabL / 2
        for sx in (+1, -1):
            boxp([sx * W / 2 * 0.94, cabY, zc],
                 [0.012 * L, cabH * 0.32, cabL * 0.36], WINDOW, cabL * cabH * 0.6)
        for sz in (+1, -1):
            boxp([0, cabY, zc + sz * cabL / 2 * 0.96],
                 [W / 2 * 0.7, cabH * 0.32, 0.012 * L], WINDOW, W * cabH * 0.6)
        if kind == 'truck':
            boxp([0, y0 + bodyH + cabH * rng.uniform(0.5, 1.0),
                  -L * rng.uniform(0.05, 0.15)],
                 [W / 2, cabH * rng.uniform(0.5, 1.0), L * 0.3], VBODY, L * W)
        nw = 4 if kind == 'car' else int(rng.choice([4, 6]))
        zs = np.linspace(-L / 2 * 0.72, L / 2 * 0.72, nw // 2)
        for sx in (+1, -1):
            for z in zs:
                blob([sx * W / 2, wheelR, z], wheelR, WHEEL, wheelR * wheelR * 8,
                     np.array([0.35, 1, 1]))
    elif kind == 'plane':
        L = rng.uniform(1.0, 1.6); fr = L * rng.uniform(0.07, 0.13)
        y0 = fr * rng.uniform(1.5, 3.0)
        parts.append((lambda n, a=[0, y0, -L / 2], b=[0, y0, L / 2], r=fr:
                      capsule_surf(a, b, r, n, rng), VBODY, L * fr * 3))
        span = L * rng.uniform(0.5, 0.9); chord = L * rng.uniform(0.12, 0.22)
        zw = L * rng.uniform(-0.1, 0.15)
        for sx in (+1, -1):
            boxp([sx * (span / 2 + fr * 0.5), y0, zw],
                 [span / 2, 0.015 * L, chord / 2], WING, span * chord)
            boxp([sx * (span * 0.18 + fr * 0.4), y0 + fr * 0.3, -L / 2 * 0.92],
                 [span * 0.18, 0.012 * L, chord * 0.3], WING, span * chord * 0.2)
        boxp([0, y0 + fr + span * 0.12, -L / 2 * 0.94],
             [0.012 * L, span * 0.12, chord * 0.35], WING, span * chord * 0.2)
        if rng.random() < 0.6:                           # nose prop
            pr = fr * rng.uniform(1.8, 3.0)
            boxp([0, y0, L / 2 + fr * 0.4], [pr, 0.03 * L, 0.02 * L], ROTOR, pr)
            boxp([0, y0, L / 2 + fr * 0.4], [0.03 * L, pr, 0.02 * L], ROTOR, pr)
        blob([0, y0 + fr * 0.75, L * 0.18], fr * 0.75, WINDOW, fr * fr * 3,
             np.array([0.8, 0.6, 1.4]))
        if rng.random() < 0.5:                           # landing gear
            for sx, z in ((+1, zw), (-1, zw), (0, L / 2 * 0.75)):
                blob([sx * span * 0.12, fr * 0.5, z], fr * 0.45, WHEEL, fr * fr * 2,
                     np.array([0.4, 1, 1]))
    else:                                                # helicopter
        L = rng.uniform(0.7, 1.1); br = L * rng.uniform(0.16, 0.24)
        y0 = br * rng.uniform(1.6, 2.4)
        blob([0, y0, L * 0.15], br, VBODY, br * br * 8,
             np.array([0.8, 0.85, 1.3]))
        parts.append((lambda n, a=[0, y0 + br * 0.2, 0], b=[0, y0 + br * 0.35, -L], r=br * 0.22:
                      capsule_surf(a, b, r, n, rng), VBODY, L * br))
        rr = L * rng.uniform(0.55, 0.85)
        boxp([0, y0 + br * 1.25, L * 0.1], [rr, 0.015 * L, 0.035 * L], ROTOR, rr)
        boxp([0, y0 + br * 1.25, L * 0.1], [0.035 * L, 0.015 * L, rr], ROTOR, rr)
        boxp([br * 0.28, y0 + br * 0.4, -L], [0.012 * L, rr * 0.22, rr * 0.22],
             ROTOR, rr * 0.2)
        blob([0, y0 + br * 0.15, L * 0.15 + br * 0.75], br * 0.6, WINDOW,
             br * br * 3, np.array([0.9, 0.7, 0.9]))
        for sx in (+1, -1):                              # skids
            parts.append((lambda n, a=[sx * br * 0.7, br * 0.25, -L * 0.25],
                          b=[sx * br * 0.7, br * 0.25, L * 0.55], r=br * 0.08:
                          capsule_surf(a, b, r, n, rng), WHEEL, L * br * 0.3))
    return parts


# --- synthetic buildings (wall/roof/window/door/chimney/foundation) ----------
def make_building(rng):
    parts = []
    kind = rng.choice(['house', 'tower', 'hut'], p=[0.55, 0.30, 0.15])

    def boxp(c, half, label, w):
        parts.append((lambda n, c=np.asarray(c, float), h=np.asarray(half, float):
                      box_surf(c, h, n, rng), label, w))

    W = rng.uniform(0.5, 1.0); D = W * rng.uniform(0.6, 1.4)
    if kind == 'tower':
        H = W * rng.uniform(2.0, 4.0); D = W * rng.uniform(0.8, 1.2)
    elif kind == 'hut':
        H = W * rng.uniform(0.4, 0.8)
    else:
        H = W * rng.uniform(0.6, 1.3)
    boxp([0, H / 2, 0], [W / 2, H / 2, D / 2], WALL, W * H * 4)

    ov = rng.uniform(1.02, 1.2)                          # roof overhang
    roofH = W * rng.uniform(0.25, 0.7) * (1.6 if kind == 'hut' else 1.0)
    flat_roof = kind == 'tower' and rng.random() < 0.5
    if flat_roof:
        boxp([0, H + W * 0.04, 0], [W / 2 * ov, W * 0.04, D / 2 * ov],
             ROOF, W * D * 2)
    elif rng.random() < 0.45 or kind == 'hut':           # pyramid roof (4 tris)
        apex = np.array([0, H + roofH, 0])
        cs = [np.array([sx * W / 2 * ov, H, sz * D / 2 * ov])
              for sx, sz in ((+1, +1), (-1, +1), (-1, -1), (+1, -1))]
        for i in range(4):
            parts.append((lambda n, a=cs[i], b=cs[(i + 1) % 4], c=apex:
                          tri_surf(a, b, c, n, rng), ROOF, W * roofH))
    else:                                                # gable roof
        ridge0 = np.array([0, H + roofH, -D / 2 * ov])
        ridge1 = np.array([0, H + roofH, D / 2 * ov])
        for sx in (+1, -1):
            eave0 = np.array([sx * W / 2 * ov, H, -D / 2 * ov])
            parts.append((lambda n, o=eave0, e1=ridge0 - eave0,
                          e2=[0, 0, D * ov]: quad_surf(o, e1, e2, n, rng),
                          ROOF, W * D))
        for sz in (+1, -1):                              # gable triangles
            parts.append((lambda n,
                          a=[+W / 2, H, sz * D / 2], b=[-W / 2, H, sz * D / 2],
                          c=[0, H + roofH, sz * D / 2]: tri_surf(a, b, c, n, rng),
                          WALL, W * roofH * 0.5))
    # windows: proud thin panes in rows on ±X and ±Z faces
    rows = max(1, int(H / (W * 0.55))) if kind != 'hut' else 1
    for row in range(rows):
        wy = H * (row + 0.55) / (rows + 0.35)
        wh = min(W, H / rows) * rng.uniform(0.12, 0.22)
        for sx in (+1, -1):
            for z in np.linspace(-D * 0.28, D * 0.28, rng.integers(1, 4)):
                boxp([sx * W / 2, wy, z], [0.015 * W, wh, wh], BWINDOW, wh * wh * 8)
        for sz in (+1, -1):
            for x in np.linspace(-W * 0.28, W * 0.28, rng.integers(1, 4)):
                boxp([x, wy, sz * D / 2], [wh, wh, 0.015 * W], BWINDOW, wh * wh * 8)
    doorH = min(H * 0.7, W * rng.uniform(0.35, 0.55))
    boxp([rng.uniform(-W * 0.2, W * 0.2), doorH / 2, D / 2],
         [doorH * 0.35, doorH / 2, 0.02 * W], DOOR, doorH * doorH * 2)
    if rng.random() < 0.4 and not flat_roof:
        boxp([rng.uniform(-W * 0.3, W * 0.3), H + roofH * rng.uniform(0.6, 1.1),
              rng.uniform(-D * 0.25, D * 0.25)],
             [W * 0.06, roofH * 0.5, W * 0.06], CHIMNEY, W * roofH * 0.3)
    if rng.random() < 0.5:
        boxp([0, W * 0.03, 0], [W / 2 * 1.15, W * 0.03, D / 2 * 1.15],
             FOUNDATION, W * D)
    return parts


def detach_parts(P, L, rng, prob=0.25):
    """Real-world exports often ship parts DETACHED from the main shape —
    wheels as separate nodes dropped below the hull, foliage clusters at odd
    offsets. Verified failure mode: a car with wheels floating under the body
    stretched the normalised box and flipped the category classifier to
    body/vegetation. With probability `prob`, offset each minority-label
    cluster by a random (downward-biased) vector so the classifier and the
    segmenters tolerate disconnected floating parts."""
    if rng.random() >= prob:
        return P
    out = P.copy()
    labs, counts = np.unique(L, return_counts=True)
    main = labs[counts.argmax()]
    ext = float(np.max(out.max(0) - out.min(0)))
    for l in labs:
        if l == main or rng.random() < 0.5:
            continue
        d = rng.normal(size=3); d /= np.linalg.norm(d) + 1e-9
        d[1] = -abs(d[1])                      # bias downward (dropped wheels)
        out[L == l] += (d * rng.uniform(0.15, 0.6) * ext).astype(np.float32)
    return out


def sample_category_cloud(rng, make_fn, n_out=N_BASE, detach_prob=0.25):
    """Like sample_body but for the non-body category plans: uniform density
    randomisation (no body-specific head-density boost), detached-part
    augmentation, same augment + normalise. Mirror is applied by the caller
    (no L/R label swap needed)."""
    parts = make_fn(rng)
    ws = np.array([w * rng.uniform(0.5, 2.5) for _, _, w in parts])
    ws /= ws.sum()
    counts = np.maximum(8, (ws * n_out * 1.5).astype(int))
    pts, lab = [], []
    for (fn, l, _), cnt in zip(parts, counts):
        pts.append(fn(int(cnt))); lab.append(np.full(int(cnt), l, np.int64))
    P = np.concatenate(pts).astype(np.float32); L = np.concatenate(lab)
    P = detach_parts(P, L, rng, detach_prob)
    P = augment(P, rng)
    idx = rng.choice(len(P), n_out, replace=len(P) < n_out)
    return normalise(P[idx]), L[idx]


CATEGORY_MAKERS = {
    "vegetation": make_tree,
    "vehicle": make_vehicle,
    "building": make_building,
}


def gen_dataset_category(category, samples, seed):
    """Synthetic dataset for a non-body category (mirror = plain x-flip)."""
    rng = np.random.default_rng(seed)
    make_fn = CATEGORY_MAKERS[category]
    aP = np.zeros((samples, N_BASE, 3), np.float32)
    aL = np.zeros((samples, N_BASE), np.int64)
    for i in range(samples):
        P, L = sample_category_cloud(rng, make_fn)
        if rng.random() < 0.5:
            P = P.copy(); P[:, 0] = -P[:, 0]
        aP[i] = P; aL[i] = L
    return aP, aL


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
        return P, L, False, "no leg labels — cannot infer up axis", 0.0
    legs_c = np.mean(legs, axis=0)
    body_c = cent.get(TORSO, cent.get(HEAD))
    if body_c is None:
        return P, L, False, "no torso/head labels", 0.0
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
        return P, L, False, "no bilateral limb pair / degenerate axes", 0.0
    Pc, score = max(frames, key=lambda t: t[1])
    # require BOTH a coherent head (above legs or forward) and most leg points
    # below the body — incoherent clouds would only inject label noise
    if score < 0.95:
        return P, L, False, f"incoherent after canonicalisation (score {score:.2f})", 0.0

    # geometric side reassignment (bind poses are bilaterally lateralised);
    # sidefix = fraction of limb points whose MINED side disagreed with the
    # geometry — an independent miner-quality signal surfaced by --check-real
    Lc = L.copy()
    relabelled = 0; limbTotal = 0
    for a, b in ((LARM, RARM), (LLEG, RLEG)):
        m = (Lc == a) | (Lc == b)
        if m.sum() == 0: continue
        xm = Pc[m, 0] - np.median(Pc[:, 0])
        new = np.where(xm >= 0, a, b)
        relabelled += int((new != Lc[m]).sum()); limbTotal += int(m.sum())
        Lc[m] = new
    sidefix = relabelled / limbTotal if limbTotal else 0.0
    return normalise(Pc), Lc, True, "", sidefix


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
        P, L, ok, msg, _ = canonicalise(P, L, f)
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
        Pc, Lc, ok, msg, sidefix = canonicalise(P, L, f)
        name = os.path.basename(f)[:44]
        if not ok:
            print(f"FAIL {name:44s} {msg}"); bad += 1; continue
        hy = Pc[Lc == HEAD, 1].mean() if (Lc == HEAD).any() else float("nan")
        hz = Pc[Lc == HEAD, 2].mean() if (Lc == HEAD).any() else float("nan")
        ly = Pc[(Lc == LLEG) | (Lc == RLEG), 1].mean()
        # The pass criterion is GEOMETRY-ONLY: head above the legs (humanoid)
        # or forward at +Z (animal). Post-canonicalisation L/R centroids are
        # NOT tested — canonicalise() rewrites sides geometrically, so they
        # hold by construction. The independent miner-quality signal is
        # `sidefix`: the fraction of limb points whose MINED side label
        # disagreed with the geometric side (high values → suspect rig
        # naming; a whole-file swap shows up as sidefix ≈ 1.0).
        good = np.isnan(hy) or hy > ly or hz > 0.05
        if not good: bad += 1
        print(f"{'ok  ' if good else 'BAD '}{name:44s} head_y={hy:+.2f} head_z={hz:+.2f} "
              f"legs_y={ly:+.2f} sidefix={sidefix:5.1%}")
    print(f"\n{len(files) - bad}/{len(files)} canonicalised cleanly")


# --- Auto-dispatch category classifier ----------------------------------------
def train_classifier(a, torch, nn):
    """Train + export meshseg_category.onnx: point cloud [1,N,3] → [1,4] logits
    over CLASSIFIER_CLASSES. Tiny PointNet (per-point MLP + max-pool + head) —
    no kNN blocks needed for a 4-way whole-cloud decision. Mined real rigs
    (--real-data) are mixed in as extra `body` samples."""
    per = max(1, a.samples // len(CLASSIFIER_CLASSES))
    rng = np.random.default_rng(a.seed)
    Ps, Ys = [], []
    print(f"generating classifier data… ({per} samples/class)")
    for ci, cls in enumerate(CLASSIFIER_CLASSES):
        for _ in range(per):
            if cls == "body":
                P, _ = sample_body(rng)
            else:
                # Stronger detachment than the segmenters: real exports drop
                # wheels/parts at odd offsets and the CLASSIFIER must shrug.
                P, _ = sample_category_cloud(rng, CATEGORY_MAKERS[cls],
                                             detach_prob=0.5)
            if rng.random() < 0.5:
                P = P.copy(); P[:, 0] = -P[:, 0]
            # The category decision must be YAW-INVARIANT (segmenters learn
            # facing; the classifier must not depend on it) — spin every
            # cloud by a full random yaw, then re-normalise.
            yaw = rng.uniform(0, 2 * np.pi)
            cy, sy = np.cos(yaw), np.sin(yaw)
            R = np.array([[cy, 0, sy], [0, 1, 0], [-sy, 0, cy]], np.float32)
            P = normalise(P @ R.T)
            Ps.append(P); Ys.append(ci)
    if a.real_data:
        rP, _ = load_real_data(a.real_data, min(a.real_aug, 7), a.seed,
                               exclude=a.val_real)
        for i in range(len(rP)):
            Ps.append(rP[i]); Ys.append(CLASSIFIER_CLASSES.index("body"))
        print(f"added {len(rP)} mined real clouds as `body` samples")
    P = torch.tensor(np.stack(Ps)); Y = torch.tensor(np.array(Ys, np.int64))
    sh = torch.randperm(len(P), generator=torch.Generator().manual_seed(a.seed))
    P, Y = P[sh], Y[sh]
    n = len(P); nval = max(1, n // 10)
    dev = "mps" if torch.backends.mps.is_available() else \
          ("cuda" if torch.cuda.is_available() else "cpu")
    print(f"classifier data n={n} dev={dev}")

    nCls = len(CLASSIFIER_CLASSES)

    class PointCls(nn.Module):
        def __init__(s, d=128):
            super().__init__()
            s.mlp = nn.Sequential(nn.Linear(3, 64), nn.GELU(),
                                  nn.Linear(64, d), nn.GELU(),
                                  nn.Linear(d, d), nn.GELU())
            s.head = nn.Sequential(nn.Linear(d, 64), nn.GELU(), nn.Linear(64, nCls))

        def forward(s, pts):                       # pts: [B,N,3]
            f = s.mlp(pts)
            return s.head(f.max(dim=1).values)     # [B,nCls]

    net = PointCls().to(dev)
    lossf = nn.CrossEntropyLoss()
    epochs = max(8, a.epochs // 3)
    opt = torch.optim.AdamW(net.parameters(), lr=2e-3, weight_decay=1e-4)
    sch = torch.optim.lr_scheduler.CosineAnnealingLR(opt, epochs)
    g = torch.Generator().manual_seed(a.seed)
    bs = max(8, a.batch * 2)
    for ep in range(epochs):
        net.train()
        perm = torch.randperm(n - nval, generator=g) + nval
        last = 0.0
        for b in range(0, perm.numel(), bs):
            bi = perm[b:b + bs]
            # random 1024-pt subsets: fast + free augmentation
            sub = torch.randint(0, N_BASE, (len(bi), 1024), generator=g)
            pts = torch.gather(P[bi], 1, sub.unsqueeze(-1).expand(-1, -1, 3))
            loss = lossf(net(pts.to(dev)), Y[bi].to(dev))
            opt.zero_grad(); loss.backward(); opt.step()
            last = loss.item()
        sch.step()
        net.eval()
        with torch.no_grad():
            correct = 0
            for b in range(0, nval, 16):
                pv = P[b:b + 16].to(dev)
                correct += (net(pv).argmax(-1) == Y[b:b + 16].to(dev)).sum().item()
        print(f"cls ep{ep:3d} loss{last:.4f} val_acc={correct / nval:.4f}", flush=True)

    out = a.out or "meshseg_category.onnx"
    net.eval().cpu()
    torch.onnx.export(
        net, (torch.zeros(1, N_BASE, 3),), out,
        input_names=["points"], output_names=["logits"],
        dynamic_axes={"points": {1: "N"}},
        opset_version=17, dynamo=False)
    print("wrote", out)


# --- training -----------------------------------------------------------------
def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--category", default="body",
                    choices=[*CATEGORIES.keys(), "classifier"],
                    help="which model to train: a per-category segmenter "
                         "(body = the original meshseg.onnx) or the Auto-dispatch "
                         "point-cloud category `classifier`")
    ap.add_argument("--samples", type=int, default=4000)
    ap.add_argument("--epochs", type=int, default=40, help="phase-1 epochs @2048 pts")
    ap.add_argument("--epochs2", type=int, default=6, help="phase-2 epochs @4096 pts")
    ap.add_argument("--batch", type=int, default=8)
    ap.add_argument("--out", default=None,
                    help="output path (default: the category's canonical file name)")
    ap.add_argument("--seed", type=int, default=0)
    ap.add_argument("--real-data", nargs="*", default=[],
                    help="dirs/files of `qtmesh segment --dump-training-data` JSON "
                         "samples (rig-prior ground truth) to MIX with synthetic data "
                         "(body segmenter + classifier only)")
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

    if a.category == "classifier":
        train_classifier(a, torch, nn); return

    nC = CATEGORIES[a.category][0]
    if a.out is None:
        a.out = CATEGORIES[a.category][1]

    print(f"generating synthetic data… (category: {a.category})")
    vP = vL = None
    if a.category == "body":
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
        if a.real_data and a.val_real:
            vP, vL = load_real_data(a.real_data, 0, a.seed, only=a.val_real)
            if not len(vP): vP = vL = None
    else:
        P, L = gen_dataset_category(a.category, a.samples, a.seed)

    P = torch.tensor(P); L = torch.tensor(L)
    n = P.shape[0]
    nval = max(1, n // 20)
    dev = "mps" if torch.backends.mps.is_available() else \
          ("cuda" if torch.cuda.is_available() else "cpu")
    print(f"data n={n} stored_points={N_BASE} dev={dev}")

    # class weights: unknown masked out, others inverse-sqrt frequency
    freq = np.bincount(L.numpy().ravel(), minlength=nC).astype(np.float64)
    w = np.zeros(nC); nz = freq > 0
    w[nz] = 1.0 / np.sqrt(freq[nz]); w[0] = 0.0
    w = w / w[nz & (np.arange(nC) > 0)].mean()
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
            s.head = nn.Sequential(nn.Linear(3 * d, d), nn.GELU(), nn.Linear(d, nC))

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
                loss = lossf(logits.reshape(-1, nC), lab.to(dev).reshape(-1))
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
