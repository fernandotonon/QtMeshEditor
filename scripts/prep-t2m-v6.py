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
import subprocess
import sys
import tempfile

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
# actions whose real-world joint speed exceeds the walk-tuned energy ceiling
FAST_ACTIONS = {"run", "march", "jump", "kick", "punch", "boxing", "attack",
                "throw", "dance"}
# minimum dot(travel, body-forward) for a locomotion window to be kept
MIN_TRAVEL_FORWARD = 0.15
# minimum ankle-scissor autocorrelation for a locomotion window (see
# gait_periodicity); real Mixamo Walk.fbx scores 0.968
MIN_PERIODICITY = 0.6
# real Mixamo clips used as the ground-truth shape for each action
REFERENCE_CLIPS = {"walk": "Walk.fbx", "run": "Running.fbx"}
# max geodesic distance (rad/joint) from the reference. Calibrated from the
# data: at 0.8 the pre-period-gate cache keeps 149 walk / 16 run windows, and
# real DIFFERENT motions sit 0.66 (jump) to 1.19 (punch) away, so 0.8 admits
# only genuinely walk-shaped motion.
MAX_REFERENCE_DISTANCE = 0.8
# plausible gait-cycle lag window in frames at 30 fps (0.67 .. 1.0 s)
GAIT_LAG_MIN = 20
GAIT_LAG_MAX = 30
# max tolerated BACKWARD bend (degrees) for the knee/elbow hinges; small
# positive slack absorbs canonicalisation noise near full extension
MAX_HYPEREXTEND_KNEE = 20.0
MAX_HYPEREXTEND_ELBOW = 35.0
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


def travel_forward(w):
    """dot(direction of travel, body's own forward). >0 = moving forward.

    Travel is inferred from the STANCE foot: a planted foot drifts backward
    relative to a body moving forward, so travel = -velocity of the slower
    (planted) foot per frame. Forward comes from the shoulder line
    (left - right) x up.

    Some source clips genuinely walk BACKWARD, and they formed a real second
    mode in the cache (37% of walk windows, mean -0.810). Flow matching then
    samples both modes, so the model walked backward about half the time
    (#837, user-reported "moving backwards"). Gating on this collapses the
    conditional to one mode.
    """
    # NB roles 7/11 are RIGHT/LEFT respectively (D_CANON[7] = -X, D_CANON[11]
    # = +X), so `side` below is right->left... which is why the cross product
    # with +Y yields the FORWARD axis with these operands in this order. A
    # review flagged the old `lsh, rsh = fk_pos(w, 7), fk_pos(w, 11)` naming as
    # a swapped binding and proposed exchanging them; that would INVERT the sign
    # and make the gate reject every genuine forward walk. Verified empirically:
    # the real Mixamo walk scores +0.581 and 100% of cached walk/run windows are
    # positive as written. Renamed to match reality rather than changing it.
    rsh, lsh = fk_pos(w, 7), fk_pos(w, 11)
    side = rsh - lsh
    side = side / (np.linalg.norm(side, axis=-1, keepdims=True) + 1e-9)
    up = np.broadcast_to(np.array([0, 1, 0], np.float32), side.shape)
    f = np.cross(side, up)
    f = (f / (np.linalg.norm(f, axis=-1, keepdims=True) + 1e-9)).mean(0)
    la, ra = fk_pos(w, 17), fk_pos(w, 21)
    vl, vr = np.diff(la, axis=0), np.diff(ra, axis=0)
    sl, sr = np.linalg.norm(vl, axis=-1), np.linalg.norm(vr, axis=-1)
    v = np.where((sl < sr)[:, None], vl, vr)
    t = -v.mean(0)
    n = np.linalg.norm(t)
    if n < 1e-6:
        return 0.0
    fn = np.linalg.norm(f)
    if fn < 1e-6:
        return 0.0
    return float(np.dot(t / n, f / fn))


def fk_pos(w, role):
    """World position of `role` over the window via the canonical chain."""
    T = len(w)
    p = np.zeros((T, 3), np.float32)
    r = role
    while PAR[r] >= 0:
        p = p + qrot(w[:, PAR[r]], np.broadcast_to(D_CANON[r], (T, 3)))
        r = PAR[r]
    return p


# ---- reference-distance gate (v7.6) ----
# The decisive measurement of this whole effort: scored against real Mixamo
# clips, the corpus's own "walk" windows sit a median 1.73 rad from Walk.fbx —
# FURTHER than a real punch (1.19), a real dance (0.91) or a real run (0.83).
# Real-vs-real is 0.03..0.44. So the corpus does not contain Mixamo-style
# walking, and every model trained on it faithfully reproduced that: the model
# measured 1.89 against its data's 1.73. No loss, gate or sampling change can
# close a gap that lives in the data. But a walk-LIKE subset exists (149 windows
# within 0.8 rad), so gate on distance to the real clips and train on that.
_REF_CACHE = {}
_REF_WARNED = set()


def _reference_windows(action, T):
    """Cycle-extended canonical windows of the real reference clip, or None."""
    if action in _REF_CACHE:
        return _REF_CACHE[action]
    fname = REFERENCE_CLIPS.get(action)
    if not fname:
        _REF_CACHE[action] = None
        return None
    path = os.path.expanduser(os.path.join("~/Downloads", fname))
    if not os.path.exists(path):
        _REF_CACHE[action] = None
        return None
    cache_dir = os.path.join(tempfile.gettempdir(), "t2m_refcache")
    os.makedirs(cache_dir, exist_ok=True)
    out = os.path.join(cache_dir,
                       os.path.basename(path).replace(" ", "_") + ".canon.json")
    if not os.path.exists(out):
        subprocess.run(["qtmesh", "anim", path, "--dump-canonical", out],
                       capture_output=True, text=True)
    if not os.path.exists(out):
        _REF_CACHE[action] = None
        return None
    try:
        clips = json.load(open(out)).get("clips", [])
        c = clips[0]
        cq, valid = prep5.canonicalize(np.asarray(c["quats"], np.float32),
                                       c["restWorld"], c["restDir"])
    except Exception:
        _REF_CACHE[action] = None
        return None
    n = len(cq)
    reps = int(np.ceil((T * 2) / max(n, 1))) + 1
    ext = np.concatenate([cq] * reps, axis=0)
    wins = [ext[s:s + T] for s in range(0, min(len(ext) - T, max(n, T)) + 1, 5)]
    _REF_CACHE[action] = (wins, valid)
    return _REF_CACHE[action]


def reference_distance(action, w):
    """Min geodesic distance to the real reference clip; inf if no reference.

    Minimised over reference windows and over cyclic shifts of `w`, so phase
    is not penalised — only the shape of the motion.
    """
    got = _reference_windows(action, len(w))
    if not got:
        return float("inf")
    wins, valid = got
    m = valid[None, :]
    denom = max(float(m.sum()) * w.shape[0], 1e-6)
    best = float("inf")
    for sh in range(0, len(w), 6):
        g = np.roll(w, sh, axis=0)
        for r in wins:
            dot = np.abs((g * r).sum(-1)).clip(0.0, 1.0)
            d = float((2.0 * np.arccos(dot) * m).sum() / denom)
            if d < best:
                best = d
    return best


def gait_periodicity(w):
    """Strongest autocorrelation of the ankle-scissor signal at lag >= 8.

    THE quality the other gates cannot see. A real gait is strongly cyclic —
    real Mixamo Walk.fbx scores 0.968 — but every other metric here (energy,
    stride ratio, travel, uprightness) is an average or a correlation that
    NON-cyclic twitching satisfies just as well. The v6.8 model measured
    healthy on all of them yet rendered as trembling in place, and its
    periodicity was 0.26 against a data median of 0.446.

    1.0 = perfect cycle, 0 = no repeat structure.
    """
    la = fk_pos(w, 17)[:, 2]
    ra = fk_pos(w, 21)[:, 2]
    sig = la - ra
    n = len(sig)
    best = -1.0
    # Search only PLAUSIBLE GAIT CADENCES. A lag floor of 8 frames (0.27 s) was
    # a serious bug: the corpus's median best-lag came out at exactly 8, i.e.
    # the gate was accepting high-frequency WOBBLE as "periodic" and the model
    # dutifully learned it — the trembling the user reported. Restricted to
    # 20..30 frames (0.67..1.0 s at 30 fps, one full stride) the real Mixamo
    # walk scores 0.968 while the corpus median is -0.379: the data is
    # ANTI-correlated at true gait cadence.
    # Pearson correlation computed PER OVERLAP. Normalising by the whole
    # window's std while averaging over the (n - lag)-sample overlap is not a
    # correlation and is not bounded: it scored a model at 1.131, above the real
    # walk's 0.968, because the signal happened to be larger inside the overlap.
    # An unbounded score also lets a single large arc pass as a "cycle".
    for lag in range(GAIT_LAG_MIN, min(GAIT_LAG_MAX + 1, n // 2 + 1)):
        a = sig[:-lag]
        b = sig[lag:]
        a = a - a.mean()
        b = b - b.mean()
        da, db = float(a.std()), float(b.std())
        if da < 1e-6 or db < 1e-6:
            continue
        c = float((a * b).mean() / (da * db))
        if c > best:
            best = c
    return max(best, 0.0)


def joint_hinge_signs(w):
    """(worst knee, worst elbow) hinge angle, degrees. NOT CURRENTLY GATED ON.

    Requested as a guard against knees/elbows bending backwards, but it does
    not work in the canonical frame and is left here documented rather than
    silently enabled. Two sign conventions were tried and both FAILED against
    real Mixamo clips:
      - sign from the lateral axis (shoulder-left minus shoulder-right): that
        axis flips between rigs, so 90% of the corpus read POSITIVE while the
        real clip read negative — the gate would have dropped 90% of the data
        (100% of march) on a convention mismatch alone.
      - sign from "the shin swings backward relative to the thigh": false,
        because mid-stride the shin legitimately swings AHEAD of the thigh; the
        real walk then scored 75.7 and the real run 131.5, i.e. rejected.
    An unsigned bound cannot work either: a real knee flexes 7..76 deg on a
    walk and to 131 deg on a run, so "bent 75 forward" and "bent 75 backward"
    are the same magnitude. A correct guard needs each rig's own bind-pose
    hinge AXIS, which the canonical representation does not carry — it would
    have to be computed in AnimationMerger against the target skeleton at
    retarget time, not here.

    Knees and elbows are HINGES — they bend one way only. A negative value is
    natural flexion in this frame and a positive one is HYPEREXTENSION (the
    joint bending backwards), which reads as a broken limb.

    Convention measured on real Mixamo clips: knees run -75..-7 deg on a walk
    and to -131 deg on a run (always negative = flexion); elbows sit near zero
    while the arms hang. So the guard is on POSITIVE knee/elbow angles.
    """
    def wdir(role):
        d = qrot(w[:, role], np.broadcast_to(D_CANON[role], (len(w), 3)))
        return d / (np.linalg.norm(d, axis=-1, keepdims=True) + 1e-9)

    # RIG-INDEPENDENT sign. A first cut took the sign from the lateral axis
    # (shoulder-left minus shoulder-right), but that axis flips between rigs:
    # 90% of the corpus scored a POSITIVE worst-knee while the real Mixamo clip
    # scored negative, so the gate would have rejected 90% of the data (100% of
    # march) purely from a convention mismatch.
    #
    # Anatomy gives a frame-free reference instead: a knee flexes so the SHIN
    # swings BACKWARD relative to the thigh, i.e. the lower segment gains a
    # component OPPOSITE the body's forward direction. Same for the forearm at
    # the elbow. Forward comes from the shoulder line crossed with up, which is
    # sign-stable because it is defined by the canonical +Y, not by which
    # shoulder is "left".
    up = np.array([0, 1, 0], np.float32)
    lsh = qrot(w[:, 11], np.broadcast_to(D_CANON[11], (len(w), 3)))
    rsh = qrot(w[:, 7], np.broadcast_to(D_CANON[7], (len(w), 3)))
    lat = lsh - rsh
    lat = lat / (np.linalg.norm(lat, axis=-1, keepdims=True) + 1e-9)
    fwd = np.cross(lat, np.broadcast_to(up, lat.shape))
    fwd = fwd / (np.linalg.norm(fwd, axis=-1, keepdims=True) + 1e-9)

    def hyperextension(upper, lower):
        """Degrees the hinge opens the WRONG way (0 when flexing naturally)."""
        u, l = wdir(upper), wdir(lower)
        ang = np.degrees(np.arccos(np.clip((u * l).sum(-1), -1, 1)))
        # component of the lower segment along +forward, relative to the upper:
        # flexion moves it backward (negative), hyperextension forward.
        rel = ((l - u) * fwd).sum(-1)
        # only count the angle when the joint opens forward
        return np.where(rel > 0.05, ang, 0.0)

    knees = max(float(hyperextension(19, 20).max()),
                float(hyperextension(15, 16).max()))
    elbows = max(float(hyperextension(12, 13).max()),
                 float(hyperextension(8, 9).max()))
    return knees, elbows


def skip_ankle_check(action):
    """Actions where a large ankle angle is legitimate (kneeling, ground work)."""
    return action in HORIZONTAL_OK or action in ("sit", "crouch", "pray",
                                                 "kick", "climb")


def dir_of(w, role):
    """World aim of `role` over the window, unit-normalised."""
    d = qrot(w[:, role], np.broadcast_to(D_CANON[role], (len(w), 3)))
    return d / (np.linalg.norm(d, axis=-1, keepdims=True) + 1e-9)


def window_quality(action, w, valid):
    """True when the window meets the library curation bar."""
    # Energy band — mean joint rotation speed (rad/frame). The upper bound is
    # ACTION-AWARE: a real Mixamo run measures 0.187 and a real walk 0.059, so
    # the single 0.11 ceiling (tuned on walk-dominated data) systematically
    # excluded genuine running — 5 of the 12 curated `run` clips died on it
    # (0.13-0.22), which is why run trained on 24-36 windows and rendered as a
    # gentle walk. Fast actions get headroom; walk keeps the tight bound.
    dq = np.abs((w[1:] * w[:-1]).sum(-1)).clip(0, 1)
    e = float((2 * np.arccos(dq)).mean())
    hi = 0.26 if action in FAST_ACTIONS else 0.11
    if not (0.004 <= e <= hi):
        return False
    # ANATOMICAL ankle bound. The foot cannot fold perpendicular to the shin,
    # yet 100% of the curated `march` windows measured a 104.5 deg ankle bend
    # (knee a healthy 39.4 deg) — the source march templates are simply broken,
    # which is exactly the "march is a twisted mess" the user reported. Left
    # ungated it also poisons the trainer's leg-chain loss, since that term is
    # masked to ALL locomotion and would be pulled toward this geometry.
    # Real walk ankle 9.7 deg, real run 21.7 deg; 70 deg is generous headroom
    # for a genuine high-knee march or kick while still rejecting a fold.
    if not skip_ankle_check(action):
        for knee, foot in ((20, 21), (16, 17)):
            if valid[knee] and valid[foot]:
                kd = dir_of(w, knee)
                fd = dir_of(w, foot)
                cos = np.clip((kd * fd).sum(-1), -1.0, 1.0)
                if float(np.degrees(np.arccos(cos)).mean()) > 70.0:
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
        # Stride-directionality gate: catches splayed/sideways walking. EXEMPT
        # march — marching lifts the knees in place, so its fore/aft travel is
        # legitimately low (CMU march windows: median ratio 1.56, only 36% clear
        # 2.0), and applying a walk gate to it discarded 64% of the data and
        # left march with 136 windows.
        if (action != "march" and valid[17] and valid[21]
                and foot_travel_ratio(w) < 2.0):
            return False
        # REFERENCE-DISTANCE gate. Supersedes the periodicity gate, which was
        # both too strict (it discarded genuinely walk-like windows: 149 survive
        # at 0.8 rad here versus 15 in the periodicity-gated cache) and unable
        # to see whether the motion actually resembles a walk. Actions with no
        # reference clip are unaffected.
        if action in REFERENCE_CLIPS:
            rdst = reference_distance(action, w)
            if not np.isfinite(rdst):
                # No reference could be extracted (missing file / dump failure).
                # Fail OPEN rather than silently rejecting every window of this
                # action, and say so once — a silent total rejection would look
                # like a data problem instead of a setup problem.
                if action not in _REF_WARNED:
                    _REF_WARNED.add(action)
                    print(f"WARNING: no reference clip for '{action}' "
                          f"({REFERENCE_CLIPS[action]}) — distance gate SKIPPED",
                          flush=True)
            elif rdst > MAX_REFERENCE_DISTANCE:
                return False
        # Travel-direction gate (v6.4): drop windows that move BACKWARD
        # relative to the body's own forward. See travel_forward().
        #
        # The threshold is RELAXED for the data-poor actions. At 0.15 the gate
        # left run with 24 windows and march 132 (from 192/308), and the ep45
        # renders showed run and march too gentle to distinguish from a walk —
        # the sampler cannot manufacture variety from 24 windows. Walk has 2704
        # and can afford the strict gate; for run/march, merely NOT going
        # backward (> 0) is the useful signal.
        if valid[7] and valid[11] and valid[17] and valid[21]:
            floor = MIN_TRAVEL_FORWARD if action == "walk" else 0.0
            if travel_forward(w) < floor:
                return False
    return True


def mirror(w, valid):
    """Sagittal (left<->right) mirror: reflect about the X=0 plane and swap
    L/R roles.

    A reflection about the plane with unit normal n maps a quaternion
    (v, w) -> (-(v - 2(v.n)n), w) — i.e. negate the two components
    PERPENDICULAR to n and keep the one along it. The sagittal plane's normal
    is X (canonical X = left), so the correct sagittal mirror negates Y and Z
    and KEEPS X... which is what this did. But that reflection also flips the
    FORWARD (Z) axis, so every mirrored window travelled the opposite way and
    the augmentation manufactured a backward-locomotion mode for free
    (measured: 37% of walk windows travel backward, a real mode at -0.810).
    Flow matching then samples both modes and the model walks backwards about
    half the time (#837, user-reported).

    A true left<->right mirror reflects about the YZ plane (negating the LATERAL
    X axis) and swaps the L/R joint roles. Conjugating a rotation by that
    reflection, S @ R(q) @ S with S = diag(-1, 1, 1), is exactly q -> (x, -y,
    -z, w), which is what this does.

    NB an earlier revision "fixed" this to (-x, y, z, -w). That is the NEGATION
    of the same quaternion, and q and -q are the SAME rotation, so it changed
    nothing — verified numerically against the conjugation above. The transform
    below was correct all along; the note is kept so the no-op is not
    reintroduced as a fix.
    """
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
    ap.add_argument("--library-repeat", type=int, default=1,
                    help="emit each curated-library take N times (#837). The "
                         "template clips face correctly and measure near real "
                         "motion; repeating them stops the far larger CMU "
                         "corpus from drowning them.")
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
        # Admit clips shorter than T/2 WHEN THEY LOOP. Game run cycles are short
        # loops: 11 of the 12 curated `run` clips are 20-27 frames, and the old
        # T//2 (30-frame) floor discarded them before the cycle-repeat below
        # could use them — leaving run with 24 training windows and a render too
        # gentle to tell apart from a walk. Measured: 10 of those 11 loop
        # cleanly (cyc <= 0.24, most exactly 0.000), so repeating them is sound.
        # Non-looping short clips are still rejected (their repeat would jump).
        if nF < T // 2:
            if nF < 16:
                return
            dq0 = np.abs((cq[-1] * cq[0]).sum(-1)).clip(0, 1)
            if float((2 * np.arccos(dq0)).mean()) >= 0.25:
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
            # Augmented copies must clear the SAME gates as the base window.
            # They previously bypassed window_quality entirely, so 18% of the
            # "periodicity-gated" walk windows were actually below the 0.6 bar
            # (min 0.200) — retime() resamples the window and can break the
            # cycle at its edges, which is exactly the property being gated on.
            mw, mv = mirror(w, valid)
            if window_quality(action, mw, mv):
                add(action, mw, mv)
            for f in (0.85, 1.15):
                rw_ = retime(w, f)
                if window_quality(action, rw_, valid):
                    add(action, rw_, valid)

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
        dropped_roles = [0]
        for c in lib.get("clips", []):
            rw, rd = c.get("restWorld"), c.get("restDir")
            if not rw or not rd:
                continue
            if excluded(c.get("source", "")):
                continue
            cq, valid = prep5.canonicalize(
                np.asarray(c["quats"], np.float32), rw, rd)
            # --min-roles was only ever applied to the CORPUS loader, so with
            # --library-repeat making the library the dominant source the flag
            # was effectively inert (another silently-dead knob, same class as
            # the amplitude term). It matters most here: the `hey` take resolves
            # only 9 of 22 roles with ALL FOUR knee/ankle roles invalid, so the
            # anatomical ankle gate cannot even run on it and the model has no
            # leg data to learn from — it invents legs. Sorting the curated
            # takes by valid-role count reproduces the user's verdict: `hey` 9
            # and `confession` 14 are the two worst and the two they called
            # "not that good", while everything they rated GOOD has >=16.
            if int(valid.sum()) < a.min_roles:
                dropped_roles[0] += 1
                continue
            # The curated TEMPLATE clips are the only source that both renders
            # with correct facing on real rigs and measures near real motion:
            # refDist to the real Mixamo walk is 0.303-0.462 for the template
            # walks (the real-vs-real floor is 0.437 and the corpus median is
            # 2.07), and their shoulder-line lateral error is 0.519 vs the
            # corpus's 0.665 and real motion's 0.483. --library-repeat emits
            # each take that many times so this good data is not drowned by the
            # much larger CMU corpus.
            for _ in range(max(1, a.library_repeat)):
                windows(c["action"], cq, valid)
            n += 1
        print(f"library: {n} takes x{max(1, a.library_repeat)} "
              f"→ {len(mo)} windows (cum)")
        if dropped_roles[0]:
            print(f"library: dropped {dropped_roles[0]} takes below "
                  f"--min-roles {a.min_roles}")
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
    # Stamp the build command into the cache. The v7.8 npz recorded only
    # fps/vocab/canonRestDir, so reproducing it later meant inferring the flags
    # from action-overlap against the library files — slow and error-prone.
    np.savez_compressed(a.out, mo=mo, msk=msk, tk=tk,
                        vocab=np.array(vocab), fps=FPS,
                        canonRestDir=D_CANON,
                        # Plain unicode arrays, NOT dtype=object: an object
                        # array forces allow_pickle=True on every reader, and
                        # a crafted npz could then execute code on load.
                        buildArgv=np.array(sys.argv, dtype=np.str_),
                        buildFlags=np.array(json.dumps(vars(a)), dtype=np.str_))
    print(f"wrote {a.out}  mo{mo.shape}")


if __name__ == "__main__":
    main()
