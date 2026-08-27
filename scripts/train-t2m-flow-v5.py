#!/usr/bin/env python3
# ruff: noqa: E702, E741
"""Train the v5 FLOW-MATCHING text-to-motion model (#840, epic #837).

ONE-TIME OFFLINE dev tool — NOT shipped. Replaces the v4 CVAE: a conditional-
mean decoder averages an action's phase-misaligned windows into gentle motion
(the v4 training notes measured this); flow matching samples a transport path
from noise to A SINGLE MODE of the data distribution instead — the one
architectural change that separates MDM-era quality from the CVAE,
independent of data scale (#837).

Data: /tmp/t2m_v5.npz from prep-t2m-v5.py — CANONICALIZED quats (rig-
independent aim+twist against the fixed canonical T-pose, #858), per-joint
validity masks, one-hot action labels.

Architecture: small DiT-style transformer v(x_t, t, action):
  x[B,T,132] (22 joints × 6D rotation) + sinusoidal frame positions,
  conditioned on (flow time t, action embedding) via AdaLN-Zero-lite
  (per-layer scale/shift from the conditioning vector). Rectified-flow
  objective: x_t=(1−t)x0+t·x1, target v*=x1−x0, masked joint-wise MSE.

Export: ONNX graph with the EULER SAMPLER UNROLLED INSIDE (N fixed steps),
matching the shipped MotionGenerator contract exactly:
  inputs  tokens[1,V] one-hot, seed[1,Z]   (Z = T·132 flattened noise;
          MotionGenerator draws N(0,0.5) — the graph rescales ×2 to unit)
  output  motion[1,T,220]                  (per joint: tx,ty,tz=0,
          quat x,y,z,w from Gram-Schmidt 6D→R, sx,sy,sz=1)
plus t2m-vocab.json carrying the CANONICAL REFERENCE TRIPLE (restWorld =
identity, restDir = the fixed canonical T-pose directions) so model clips
ride the same bind-referenced direction retarget as v5 template clips —
retiring the synthetic-standing-pose shim (#858).

Usage:
  python3 scripts/train-t2m-flow-v5.py --data /tmp/t2m_v5.npz \
      --out /tmp/t2m_v5_flow --epochs 60 --device mps [--steps 16]
"""
import argparse
import json
import math
import os

import numpy as np
import torch
import torch.nn as nn
import torch.nn.functional as F

J, D6 = 22, 6
C6 = J * D6      # 132


# ---------- rotation reps ----------
def quat_to_6d(q):
    """q[...,4] (x,y,z,w) → first two rotation-matrix COLUMNS [...,6]."""
    x, y, z, w = q.unbind(-1)
    c0 = torch.stack([1 - 2 * (y * y + z * z),
                      2 * (x * y + z * w),
                      2 * (x * z - y * w)], -1)
    c1 = torch.stack([2 * (x * y - z * w),
                      1 - 2 * (x * x + z * z),
                      2 * (y * z + x * w)], -1)
    return torch.cat([c0, c1], -1)


def d6_to_quat(d):
    """[...,6] → unit quats [...,4] (x,y,z,w) via Gram-Schmidt (ONNX-safe)."""
    a, b = d[..., :3], d[..., 3:]
    c0 = F.normalize(a, dim=-1, eps=1e-6)
    b = b - (c0 * b).sum(-1, keepdim=True) * c0
    c1 = F.normalize(b, dim=-1, eps=1e-6)
    c2 = torch.cross(c0, c1, dim=-1)
    m00, m10, m20 = c0.unbind(-1)
    m01, m11, m21 = c1.unbind(-1)
    m02, m12, m22 = c2.unbind(-1)
    # branchless Shepperd: build all four candidates, pick the max-norm one
    t0 = 1 + m00 + m11 + m22
    t1 = 1 + m00 - m11 - m22
    t2 = 1 - m00 + m11 - m22
    t3 = 1 - m00 - m11 + m22
    eps = 1e-8
    q0 = torch.stack([m21 - m12, m02 - m20, m10 - m01, t0], -1) \
        / (2.0 * torch.sqrt(t0.clamp_min(eps)).unsqueeze(-1))
    q1 = torch.stack([t1, m01 + m10, m02 + m20, m21 - m12], -1) \
        / (2.0 * torch.sqrt(t1.clamp_min(eps)).unsqueeze(-1))
    q2 = torch.stack([m01 + m10, t2, m12 + m21, m02 - m20], -1) \
        / (2.0 * torch.sqrt(t2.clamp_min(eps)).unsqueeze(-1))
    q3 = torch.stack([m02 + m20, m12 + m21, t3, m10 - m01], -1) \
        / (2.0 * torch.sqrt(t3.clamp_min(eps)).unsqueeze(-1))
    ts = torch.stack([t0, t1, t2, t3], -1)
    idx = ts.argmax(-1, keepdim=True)
    qs = torch.stack([q0, q1, q2, q3], -2)               # [...,4cand,4]
    q = torch.gather(qs, -2,
                     idx.unsqueeze(-1).expand(*idx.shape, 4)).squeeze(-2)
    return F.normalize(q, dim=-1, eps=1e-6)


# ---------- gait-phase supervision (#837 orientation correctness) ----------
# Canonical parent chain + rest directions, mirroring prep-t2m-v5/v6 exactly.
PAR_CANON = [-1, 0, 1, 2, 3, 4, 2, 6, 7, 8, 2, 10, 11, 12,
             0, 14, 15, 16, 0, 18, 19, 20]
DIR_CANON = [                       # keep in sync with prep-t2m-v5.D_CANON
    [0, 1, 0], [0, 1, 0], [0, 1, 0], [0, 1, 0], [0, 1, 0], [0, 1, 0],
    [-1, 0, 0], [-1, 0, 0], [-1, 0, 0], [-1, 0, 0],   # roles 6-9  = RIGHT arm
    [1, 0, 0], [1, 0, 0], [1, 0, 0], [1, 0, 0],       # roles 10-13 = LEFT arm
    [0, -1, 0], [0, -1, 0], [0, -1, 0], [0, 0, 1],
    [0, -1, 0], [0, -1, 0], [0, -1, 0], [0, 0, 1],
]
# roles whose Z trajectory defines the gait phase
L_ANKLE, R_ANKLE, L_WRIST, R_WRIST = 17, 21, 9, 13


def qrot_t(q, v):
    """Rotate v[...,3] by quat q[...,4] (x,y,z,w). Differentiable."""
    qv, qw = q[..., :3], q[..., 3:]
    uv = torch.cross(qv, v, dim=-1)
    uuv = torch.cross(qv, uv, dim=-1)
    return v + 2.0 * (qw * uv + uuv)


def fk_pos(q, role, dirs):
    """World position of `role` by walking the canonical parent chain.

    q: [B,T,J,4]. Mirrors the numpy FK in prep-t2m-v6/eval-t2m-posture, so the
    quantity supervised here is the same one the metrics and the retarget read.
    """
    p = torch.zeros(q.shape[0], q.shape[1], 3, device=q.device, dtype=q.dtype)
    r = role
    while PAR_CANON[r] >= 0:
        d = dirs[r].expand(q.shape[0], q.shape[1], 3)
        p = p + qrot_t(q[:, :, PAR_CANON[r]], d)
        r = PAR_CANON[r]
    return p


def _centred_z(p):
    z = p[..., 2]
    return z - z.mean(dim=1, keepdim=True)


def travel_forward_torch(q, dirs):
    """dot(direction of travel, body forward), differentiable, per sample.

    Same quantity as prep-t2m-v6.travel_forward and the eval: travel is
    inferred from the STANCE (slower) foot, which drifts backward while the
    body moves forward. Forward is (Lshoulder - Rshoulder) x up.

    Even with a 100%-forward training set the model sat at ~50% forward
    (measured at ep40 with foot motion at 73% of the data's magnitude, so this
    is a real direction ambiguity and not measurement noise). Flow matching has
    no term tying the emitted gait to a travel direction, so this supervises it
    directly — the same reasoning as the gait-phase hinge.
    """
    lsh = fk_pos(q, 7, dirs)
    rsh = fk_pos(q, 11, dirs)
    side = F.normalize(lsh - rsh, dim=-1, eps=1e-6)
    up = torch.zeros_like(side)
    up[..., 1] = 1.0
    fwd = F.normalize(torch.cross(side, up, dim=-1), dim=-1, eps=1e-6)
    fwd = F.normalize(fwd.mean(dim=1), dim=-1, eps=1e-6)          # [B,3]

    la = fk_pos(q, 17, dirs)
    ra = fk_pos(q, 21, dirs)
    vl = la[:, 1:] - la[:, :-1]
    vr = ra[:, 1:] - ra[:, :-1]
    sl = vl.norm(dim=-1, keepdim=True)
    sr = vr.norm(dim=-1, keepdim=True)
    v = torch.where(sl < sr, vl, vr)                              # stance foot
    travel = -v.mean(dim=1)                                       # [B,3]
    travel = F.normalize(travel, dim=-1, eps=1e-6)
    return (travel * fwd).sum(-1)                                 # [B] in [-1,1]


def amplitude_excess(q, dirs, lo_scale=None):
    """How far the ankle/wrist fore-aft excursion EXCEEDS human range.

    The phase + travel hinges reward motion and nothing bounded it, so the
    model over-drove the limbs: measured stride 3.46 and armSwing 3.73 against
    real Mixamo walk values of 0.765 and 1.718 - rendering as a knee-to-chest
    exaggerated march instead of a walk. This penalises only the EXCESS above
    generous ceilings, so normal motion is untouched.
    """
    def pk(role):
        z = fk_pos(q, role, dirs)[..., 2]
        return z.max(dim=1).values - z.min(dim=1).values

    stride = 0.5 * (pk(17) + pk(21))
    swing = 0.5 * (pk(9) + pk(13))
    # TWO-SIDED, like the speed band. A ceiling-only amplitude term contributes
    # exactly 0.0 once the model is under it, so nothing defends the limbs from
    # COLLAPSING: measured at v6.7 ep45 the jitter term was 80% of the guard
    # loss (0.1446/0.181) while amp was 0.0000, and armSwing/stride fell
    # 1.481/1.126 -> 0.894/0.690, i.e. BELOW the real walk's 1.718/0.765. The
    # limbs were collateral damage of jitter pushing speed down. Real-walk
    # values anchor the floors (stride 0.765, swing 1.718); floors are set just
    # under them and weighted 2x so shrinking is punished harder than growing.
    # Ceilings sit above the ALL-ACTION p99 (stride 3.33, swing 3.54) so real
    # data is never penalised. The original 1.6 stride ceiling was BELOW the
    # data's p95 of 2.969 — it was clipping legitimate motion across march,
    # climb and kick, which contributed to the amplitude collapse.
    over = (stride - 3.5).clamp_min(0.0) + (swing - 3.7).clamp_min(0.0)
    # Floors calibrated to the TRAINING data's 5th percentile (walk stride 0.741,
    # swing 0.809) — NOT to the reference Mixamo clip, whose swing (1.718) sits
    # well above this corpus's median (1.052). Floors above the data would
    # penalise legitimate windows: at swing floor 1.50, 67% of real walk windows
    # scored a penalty.
    st_floor = 0.70 * (1.0 if lo_scale is None else lo_scale)
    sw_floor = 0.75 * (1.0 if lo_scale is None else lo_scale)
    under = ((st_floor - stride).clamp_min(0.0)
             + (sw_floor - swing).clamp_min(0.0)) * 2.0
    return over + under


def jitter_excess(q, hi=None, lo=None):
    """Per-frame angular speed ABOVE the training band = high-frequency jitter.

    THE defect that made every amplitude metric lie. The ep90 model emitted a
    mean joint speed of 0.266 rad/frame against the data's 0.0557 (band
    0.032-0.099), and its worst joint (role 21) ran at 1.416 rad/frame — about
    81 deg per frame, physically impossible. The retarget's smoothing then
    damped it to 0.041, so the RENDER was nearly static while the canonical
    ankle "scissor" measured LARGER than the real walk: the metrics were
    reading jitter amplitude, not stride.

    Penalise the mean per-joint speed above a ceiling just past the data band,
    plus the worst single joint, which is where the impossible spikes live.
    """
    dot = (q[:, 1:] * q[:, :-1]).sum(-1).abs().clamp(0.0, 1.0)
    speed = 2.0 * torch.acos(dot.clamp(max=1.0 - 1e-7))     # [B,T-1,J]
    per_joint = speed.mean(dim=1)                            # [B,J]
    # TWO-SIDED band, not a ceiling. A pure ceiling removes the jitter AND the
    # motion with it: at ep30 speed fell 0.353 -> 0.227 and the render went
    # smooth but nearly STATIC (legs barely separating, contra dropped
    # 0.345 -> 0.284). Jitter and stride were entangled, so the objective has
    # to pull the speed TOWARD the data band (walk mean 0.056, real Walk.fbx
    # 0.059) rather than merely below a ceiling.
    m = per_joint.mean(dim=-1)
    # The band is PER-SAMPLE so it can be action-aware. A single band is
    # action-blind, and after the run/march data recovery the cache's overall
    # energy rose 0.0429 -> 0.0550 (p90 0.078 -> 0.113) while WALK-only energy
    # stayed at 0.0510 — the faster fast-action windows pull the shared model
    # and walk inherits the speed, which is what degraded the v7.2 walk render.
    hi_t = 0.10 if hi is None else hi
    lo_t = 0.045 if lo is None else lo
    mean_excess = ((m - hi_t).clamp_min(0.0)
                   + (lo_t - m).clamp_min(0.0) * 4.0)
    worst_excess = (per_joint.max(dim=-1).values - 0.45).clamp_min(0.0)
    return mean_excess + worst_excess


def gait_aperiodicity(q, dirs):
    """1 - (best autocorrelation of the ankle-scissor signal), differentiable.

    The property NO other term can see. A real Mixamo walk autocorrelates at
    0.968; the v6.8 model measured 0.264 and rendered as trembling in place
    while scoring healthy on travel, contra, twist, amplitude and speed — all
    of which are averages or correlations that non-cyclic twitching satisfies.

    Gating the DATA to >= 0.6 periodicity was not enough on its own: the model
    still emitted 0.247-0.293, i.e. no better than before. Same lesson as travel
    direction — flow matching does not inherit a property just because the data
    has it. If it is not in the loss, it is not in the output.

    Autocorrelation is evaluated at a fixed lag set spanning plausible gait
    periods (10..29 frames at 30 fps = 0.33..0.97 s) and the best is taken, so
    the term is agnostic to cadence.
    """
    la = fk_pos(q, 17, dirs)[..., 2]
    ra = fk_pos(q, 21, dirs)[..., 2]
    sig = la - ra
    T = sig.shape[1]
    best = torch.full((sig.shape[0],), -1.0, device=q.device, dtype=sig.dtype)
    # Lag range must MATCH prep-t2m-v6.gait_periodicity (8 .. T//2), otherwise
    # the loss disagrees with the gate that selected the data: a narrower
    # 10..29 window scored the >=0.6-periodic training set at 0.43-0.51
    # aperiodicity because it missed the slower cadences.
    # Match prep-t2m-v6's GAIT_LAG_MIN/MAX. Searching from lag 8 rewarded
    # high-frequency wobble instead of a stride (see gait_periodicity).
    # Pearson per overlap (bounded), matching prep-t2m-v6.gait_periodicity.
    for lag in range(20, min(31, max(21, T // 2 + 1))):
        a = sig[:, :-lag]
        b = sig[:, lag:]
        a = a - a.mean(dim=1, keepdim=True)
        b = b - b.mean(dim=1, keepdim=True)
        c = ((a * b).mean(dim=1)
             / ((a.std(dim=1) * b.std(dim=1)) + 1e-4))
        best = torch.maximum(best, c)
    return (1.0 - best).clamp_min(0.0)


def spine_twist_penalty(q):
    """Mean |hip->chest yaw| in radians — an anatomy guard for the phase loss.

    The phase term alone is satisfiable by a DEGENERATE shortcut: rotate the
    torso ~180 deg so the existing swing reads as contralateral. Measured at
    --phase-weight 0.15 without this guard: hip->chest twist went to 170 deg
    (data 6.3 deg, v6.2 6.7 deg) — the user-visible "walk got all twisted".
    Penalising the yaw closes that escape route so the only way to earn the
    phase reward is to actually fix limb timing.
    """
    def conj(a):
        return torch.cat([-a[..., :3], a[..., 3:]], -1)

    def mul(a, b):
        ax, ay, az, aw = a.unbind(-1)
        bx, by, bz, bw = b.unbind(-1)
        return torch.stack([aw * bx + ax * bw + ay * bz - az * by,
                            aw * by - ax * bz + ay * bw + az * bx,
                            aw * bz + ax * by - ay * bx + az * bw,
                            aw * bw - ax * bx - ay * by - az * bz], -1)

    rel = mul(conj(q[:, :, 0]), q[:, :, 2])          # hip -> chest
    yaw = 2.0 * torch.atan2(rel[..., 1], rel[..., 3])
    # wrap to [-pi, pi] so a 180 deg cheat is maximally penalised
    yaw = torch.atan2(torch.sin(yaw), torch.cos(yaw))
    return yaw.abs().mean(dim=1)


def gait_phase_corr(q, dirs):
    """Per-sample contralateral gait correlation, in [-1, 1].

    A correct walk pairs LEFT leg forward with RIGHT arm forward, so
    corr(Lankle_z, Rwrist_z) > 0 and corr(Lankle_z, Lwrist_z) < 0. The
    training data measures +0.63 / -0.58 (92% correct sign) but the v6.1 and
    v6.2 models emit only 67% / 75% correct — samples with the arms swinging
    on the wrong side, which renders as "limbs moving as if facing backwards"
    (user-reported). Nothing in the flow-matching loss constrained this, so
    this term supervises it directly.

    Returns the mean of (contralateral - ipsilateral)/2, averaged over both
    body sides so the term is itself mirror-symmetric.
    """
    la = _centred_z(fk_pos(q, L_ANKLE, dirs))
    ra = _centred_z(fk_pos(q, R_ANKLE, dirs))
    lw = _centred_z(fk_pos(q, L_WRIST, dirs))
    rw = _centred_z(fk_pos(q, R_WRIST, dirs))

    def corr(x, y):
        xs = x / (x.std(dim=1, keepdim=True) + 1e-4)
        ys = y / (y.std(dim=1, keepdim=True) + 1e-4)
        return (xs * ys).mean(dim=1)

    # both sides, so the objective cannot prefer one chirality
    contra = 0.5 * (corr(la, rw) + corr(ra, lw))
    ipsi = 0.5 * (corr(la, lw) + corr(ra, rw))
    return 0.5 * (contra - ipsi)


# ---------- model ----------
class Block(nn.Module):
    def __init__(self, dim, heads):
        super().__init__()
        self.n1 = nn.LayerNorm(dim, elementwise_affine=False)
        self.attn = nn.MultiheadAttention(dim, heads, batch_first=True)
        self.n2 = nn.LayerNorm(dim, elementwise_affine=False)
        self.mlp = nn.Sequential(nn.Linear(dim, dim * 4), nn.GELU(),
                                 nn.Linear(dim * 4, dim))
        self.ada = nn.Linear(dim, dim * 6)
        nn.init.zeros_(self.ada.weight)
        nn.init.zeros_(self.ada.bias)

    def forward(self, x, c):
        s1, b1, g1, s2, b2, g2 = self.ada(c).unsqueeze(1).chunk(6, -1)
        h = self.n1(x) * (1 + s1) + b1
        x = x + g1 * self.attn(h, h, h, need_weights=False)[0]
        h = self.n2(x) * (1 + s2) + b2
        return x + g2 * self.mlp(h)


class FlowDiT(nn.Module):
    def __init__(self, V, T, dim=256, layers=6, heads=8):
        super().__init__()
        self.T = T
        self.inp = nn.Linear(C6, dim)
        self.pos = nn.Parameter(torch.randn(1, T, dim) * 0.02)
        self.act_emb = nn.Linear(V, dim)
        self.t_mlp = nn.Sequential(nn.Linear(dim, dim), nn.SiLU(),
                                   nn.Linear(dim, dim))
        self.blocks = nn.ModuleList([Block(dim, heads) for _ in range(layers)])
        self.out_n = nn.LayerNorm(dim, elementwise_affine=False)
        self.out = nn.Linear(dim, C6)
        nn.init.zeros_(self.out.weight)
        nn.init.zeros_(self.out.bias)
        half = dim // 2
        self.register_buffer(
            "freqs", torch.exp(-math.log(1e4)
                               * torch.arange(half).float() / half))

    def forward(self, x, t, tok):
        # x[B,T,C6], t[B] in [0,1], tok[B,V]
        ang = t[:, None] * 1000.0 * self.freqs[None]
        temb = torch.cat([torch.sin(ang), torch.cos(ang)], -1)
        c = self.t_mlp(temb) + self.act_emb(tok)
        h = self.inp(x) + self.pos
        for blk in self.blocks:
            h = blk(h, c)
        return self.out(self.out_n(h))


class Sampler(nn.Module):
    """Euler flow sampler UNROLLED for ONNX export — MotionGenerator contract."""

    def __init__(self, net, V, T, steps, guidance=2.0):
        super().__init__()
        self.net, self.V, self.T, self.steps = net, V, T, steps
        self.guidance = guidance

    def forward(self, tokens, seed):
        B = 1
        # MotionGenerator draws seed ~ N(0, 0.5) — rescale to unit noise.
        x = seed.reshape(B, self.T, C6) * 2.0
        uncond = torch.zeros_like(tokens)
        for i in range(self.steps):
            t = torch.full((B,), i / self.steps, dtype=x.dtype,
                           device=x.device)
            vc = self.net(x, t, tokens)
            vu = self.net(x, t, uncond)
            v = vu + self.guidance * (vc - vu)
            x = x + v / self.steps
        q = d6_to_quat(x.reshape(B, self.T, J, D6))       # [1,T,J,4]
        zeros3 = torch.zeros(B, self.T, J, 3, dtype=x.dtype, device=x.device)
        ones3 = torch.ones(B, self.T, J, 3, dtype=x.dtype, device=x.device)
        motion = torch.cat([zeros3, q, ones3], -1)        # [1,T,J,10]
        return motion.reshape(B, self.T, J * 10)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--data", default="/tmp/t2m_v5.npz")
    ap.add_argument("--out", default="/tmp/t2m_v5_flow")
    ap.add_argument("--epochs", type=int, default=60)
    ap.add_argument("--batch", type=int, default=256)
    ap.add_argument("--lr", type=float, default=2e-4)
    ap.add_argument("--dim", type=int, default=256)
    ap.add_argument("--layers", type=int, default=6)
    ap.add_argument("--steps", type=int, default=16, help="Euler export steps")
    # Guidance default 1.0: with posture-FILTERED training data (already
    # more upright than the uncond distribution), CFG > 1 extrapolates PAST
    # upright into a backward arch — measured on the v5.1 retrain (g2.0
    # arched, g1.0 clean). Raise only for conditioning-starved datasets.
    ap.add_argument("--guidance", type=float, default=1.0,
                    help="CFG scale baked into the exported sampler")
    ap.add_argument("--device", default="mps")
    ap.add_argument("--balance-power", type=float, default=1.0,
                    help="class-balance exponent: 1.0 = inverse-frequency "
                         "(equal per action), 0.5 = sqrt-tempered (favours "
                         "the data-rich actions like walk), 0.0 = raw")
    ap.add_argument("--phase-weight", type=float, default=0.0,
                    help="weight of the contralateral gait-phase loss "
                         "(#837 orientation correctness). 0 = off. Applied to "
                         "LOCOMOTION actions only, on the reconstructed clean "
                         "sample. 0.05-0.2 is a sane range.")
    ap.add_argument("--travel-weight", type=float, default=0.0,
                    help="weight of the travel-DIRECTION hinge: the body must "
                         "move along its own forward axis (#837). Locomotion "
                         "rows only. 0 = off; 0.1-0.3 is a sane range.")
    ap.add_argument("--jitter-weight", type=float, default=0.0,
                    help="penalise per-frame angular speed above the data band "
                         "(#837). Applied to ALL actions — jitter is never "
                         "wanted. 0 = off; 1.0 is a sane start.")
    ap.add_argument("--loco-boost", type=float, default=1.0,
                    help="multiply walk/run/march sampling weight (#837). The "
                         "periodicity gate leaves them ~7%% of batches, so the "
                         "gait losses barely reach any rows. 6.0 gives ~33%%.")
    ap.add_argument("--period-weight", type=float, default=0.0,
                    help="reward a periodic GAIT CYCLE on locomotion rows "
                         "(#837). Data gating alone does not produce one. "
                         "0 = off; 0.5 is a sane start.")
    ap.add_argument("--amp-weight", type=float, default=0.0,
                    help="penalise limb excursion ABOVE human range (#837): "
                         "the phase/travel hinges reward motion and otherwise "
                         "over-drive the legs into a knee-to-chest march. "
                         "0 = off; 0.3 is a sane start.")
    ap.add_argument("--twist-weight", type=float, default=0.5,
                    help="weight of the hip->chest yaw penalty that stops the "
                         "phase loss from cheating by rotating the torso 180 "
                         "deg. Only active when --phase-weight > 0.")
    ap.add_argument("--phase-actions", default="walk,run,march",
                    help="comma list the phase loss applies to")
    ap.add_argument("--resume", action="store_true",
                    help="resume from <out>/ckpt.pt (long runs survive "
                         "sleep/restarts)")
    a = ap.parse_args()

    d = np.load(a.data, allow_pickle=True)
    mo, msk, tk = d["mo"], d["msk"], d["tk"]
    vocab = [str(w) for w in d["vocab"]]
    fps = int(d["fps"])
    canon_rd = d["canonRestDir"]
    N, T = mo.shape[0], mo.shape[1]
    V = len(vocab)
    print(f"data: {N} windows T={T} V={V} vocab={vocab}")

    dev = torch.device(a.device if (a.device != "mps"
                                    or torch.backends.mps.is_available())
                       else "cpu")
    x1 = quat_to_6d(torch.from_numpy(mo)).reshape(N, T, C6)      # data
    m6 = torch.from_numpy(msk).repeat_interleave(D6, -1)         # [N,C6]
    tok = torch.from_numpy(tk)

    # gait-phase loss plumbing (#837): a per-action gate + the canonical rest
    # directions as a device tensor for the differentiable FK.
    dirs_t = torch.tensor(DIR_CANON, dtype=torch.float32, device=dev)
    phase_acts = {w for w in a.phase_actions.split(",") if w}
    loco_idx = [i for i, w in enumerate(vocab) if w in phase_acts]
    loco_mask_v = torch.zeros(V, device=dev)
    for i in loco_idx:
        loco_mask_v[i] = 1.0
    # actions whose real joint speed exceeds the walk band (mirrors
    # prep-t2m-v6.FAST_ACTIONS)
    # Measured mean-speed medians on the v72 cache: run 0.110, dance 0.095,
    # punch 0.064, walk 0.055, march 0.050, jump 0.042. Only run and dance
    # genuinely need a raised ceiling; march/jump are NOT fast by this metric.
    fast_names = {"run", "dance"}
    fast_mask_v = torch.zeros(V, device=dev)
    for i, w in enumerate(vocab):
        if w in fast_names:
            fast_mask_v[i] = 1.0
    if a.phase_weight > 0:
        print(f"gait-phase loss ON (w={a.phase_weight}) for "
              f"{[vocab[i] for i in loco_idx]}", flush=True)

    # Class-balanced sampling — walk dominates the window count (v4 lesson).
    # `--balance-power` tempers it: 1.0 = pure inverse-frequency (every action
    # drawn equally often), 0.5 = sqrt-tempered, 0.0 = raw distribution.
    #
    # Pure inverse-frequency badly starves the actions that matter most. On the
    # v6.2 cache walk is 30.85% of windows but only 4.35% of draws (0.14x its
    # share) while 16-window curiosities like `confession` get 67x oversampling
    # — equal capacity spent memorising 16 windows as on 7688 walk windows.
    # Measured effect: walk fwd/side plateaued at ~2.6 by ep167 while the loss
    # kept falling. sqrt-tempering gives walk 16.1% and still leaves run 2.8%.
    freq = tk.sum(0)
    w = (tk @ (1.0 / np.maximum(freq, 1.0) ** a.balance_power)).astype(np.float64)
    # LOCOMOTION BOOST. The periodicity gate shrank walk/run/march to 467/39/41
    # windows of 19693 while the other 21 actions kept theirs, so locomotion was
    # only 7.1% of sampled batches (~18 rows of 256) — the gait losses reach
    # only those rows, so they were weak in practice no matter their weight.
    # balance-power alone caps locomotion at 12.5% (3 of 24 actions at bp=1.0),
    # which is still far too little for the three actions that matter here.
    if a.loco_boost > 1.0:
        loco_names = {"walk", "run", "march"}
        li = [i for i, n in enumerate(vocab) if n in loco_names]
        if li:
            boost = np.ones(len(vocab), np.float64)
            for i in li:
                boost[i] = a.loco_boost
            w = w * (tk @ boost)
            sh = w[(tk[:, li].sum(1) > 0)].sum() / w.sum()
            print(f"locomotion sampling share: {100 * sh:.1f}% "
                  f"(boost x{a.loco_boost})", flush=True)
    sampler = torch.utils.data.WeightedRandomSampler(
        torch.from_numpy(w), num_samples=N, replacement=True)
    ds = torch.utils.data.TensorDataset(x1, m6, tok)
    dl = torch.utils.data.DataLoader(ds, batch_size=a.batch, sampler=sampler,
                                     drop_last=True)

    net = FlowDiT(V, T, dim=a.dim, layers=a.layers).to(dev)
    print("params:", sum(p.numel() for p in net.parameters()) / 1e6, "M")
    opt = torch.optim.AdamW(net.parameters(), lr=a.lr, weight_decay=1e-4)
    sched = torch.optim.lr_scheduler.CosineAnnealingLR(
        opt, T_max=a.epochs * max(1, len(dl)))

    os.makedirs(a.out, exist_ok=True)
    ckpt_path = os.path.join(a.out, "ckpt.pt")
    start_ep = 0
    if a.resume and os.path.exists(ckpt_path):
        # weights_only=True: the checkpoint is only state_dicts + an int
        # epoch — no pickled objects — so load safely (no code execution).
        ck = torch.load(ckpt_path, map_location=dev, weights_only=True)
        net.load_state_dict(ck["net"])
        opt.load_state_dict(ck["opt"])
        sched.load_state_dict(ck["sched"])
        start_ep = ck["epoch"] + 1
        print(f"resumed from epoch {start_ep}", flush=True)

    for ep in range(start_ep, a.epochs):
        # Accumulate the epoch loss ON DEVICE. A per-batch `loss.item()` is a
        # GPU->CPU sync (`_local_scalar_dense_mps` -> `waitUntilCompleted`)
        # that drains the whole MPS queue ~97x/epoch; on long runs the MPS
        # allocator degrades until each sync parks the process in
        # uninterruptible wait and throughput collapses ~11x (observed at
        # ep84: 84s/epoch -> ~16min/epoch). One sync per EPOCH instead.
        tot = torch.zeros((), device=dev)
        nb = 0
        for xb, mb, tb in dl:
            xb, mb, tb = xb.to(dev), mb.to(dev), tb.to(dev)
            x0 = torch.randn_like(xb)
            # classifier-free guidance: drop the action condition 10% of the
            # time so the sampler can extrapolate cond vs uncond at export.
            tb_raw = tb
            drop = (torch.rand(tb.shape[0], device=dev) < 0.1).float()
            tb = tb * (1.0 - drop)[:, None]
            t = torch.rand(xb.shape[0], device=dev)
            xt = (1 - t[:, None, None]) * x0 + t[:, None, None] * xb
            v = net(xt, t, tb)
            tgt = xb - x0
            mask = mb[:, None, :]                       # [B,1,C6]
            loss = ((v - tgt) ** 2 * mask).sum() / mask.sum() / T
            if a.jitter_weight > 0:
                q_all = d6_to_quat(
                    (xt + (1.0 - t[:, None, None]) * v).reshape(-1, T, J, D6))
                # per-sample band: fast actions may legitimately move ~2.5x a
                # walk (real Mixamo run 0.187 vs walk 0.059)
                # Bands are set from the DATA's per-action p95, measured on the
                # v72 cache. Note march's mean speed (median 0.050) is close to
                # WALK's (0.055) — marching in place is not fast by this measure,
                # so lumping it with run and raising its FLOOR to 0.10 penalised
                # 96% of real march windows. Only genuinely fast actions (run,
                # dance) get the raised ceiling, and the floor stays low for all.
                is_fast = (tb_raw * fast_mask_v).sum(-1).clamp(0, 1)
                hi = 0.10 + is_fast * 0.12          # walk 0.10, fast 0.22
                lo = torch.full_like(hi, 0.035)     # low floor for every action
                loss = loss + a.jitter_weight * jitter_excess(
                    q_all, hi=hi, lo=lo).mean()
            if a.phase_weight > 0:
                # Flow matching predicts a VELOCITY, so reconstruct the clean
                # sample the model implies at this t before measuring gait:
                #   x_t = (1-t)x0 + t*x1  and  v* = x1 - x0  =>  x1 = x_t + (1-t)v
                x1_hat = xt + (1.0 - t[:, None, None]) * v
                q_hat = d6_to_quat(x1_hat.reshape(-1, T, J, D6))
                ph = gait_phase_corr(q_hat, dirs_t)          # [B], want -> +1
                # gate to locomotion rows only (tb is the CFG-dropped token, so
                # use the pre-drop labels for the gate)
                g = (tb_raw * loco_mask_v).sum(-1).clamp(0, 1)
                denom = g.sum().clamp_min(1.0)
                # hinge: only penalise below a firm-but-not-saturating target,
                # so well-phased samples are left alone
                # anatomy guard: free below ~20 deg of hip->chest yaw (the
                # data sits at ~6 deg), then linearly penalised.
                tw = spine_twist_penalty(q_hat)
                tw_pen = (tw - 0.35).clamp_min(0.0)
                loss = loss + a.phase_weight * (
                    (g * (0.6 - ph).clamp_min(0.0)).sum() / denom)
                loss = loss + a.twist_weight * (
                    (g * tw_pen).sum() / denom)
                if a.period_weight > 0:
                    # WALK rows only — see prep-t2m-v6's periodicity gate: the
                    # real Mixamo run has no positive autocorrelation at any
                    # lag, so demanding it of run/march asks for something real
                    # running does not have.
                    walk_j = vocab.index("walk") if "walk" in vocab else -1
                    if walk_j >= 0:
                        gp = tb_raw[:, walk_j].clamp(0, 1)
                        ap = gait_aperiodicity(q_hat, dirs_t)
                        loss = loss + a.period_weight * (
                            (gp * ap).sum() / gp.sum().clamp_min(1.0))
                if a.amp_weight > 0:
                    # Floors are calibrated to WALK's data p5; march/run have
                    # legitimately larger excursion (march stride p5 1.174 vs
                    # walk 0.741), so a global floor penalised 81% of real march
                    # windows. Apply the floor only to walk rows; the others
                    # keep the over-drive ceiling.
                    walk_i = vocab.index("walk") if "walk" in vocab else -1
                    if walk_i >= 0:
                        gw = tb_raw[:, walk_i].clamp(0, 1)
                    else:
                        gw = torch.zeros_like(g)
                    amp_ceil = amplitude_excess(q_hat, dirs_t, lo_scale=0.0)
                    amp_full = amplitude_excess(q_hat, dirs_t)
                    amp = amp_ceil + gw * (amp_full - amp_ceil)
                    loss = loss + a.amp_weight * (
                        (g * amp).sum() / denom)
                if a.travel_weight > 0:
                    tv = travel_forward_torch(q_hat, dirs_t)
                    # hinge toward the data's level (~+0.75); no reward above it
                    loss = loss + a.travel_weight * (
                        (g * (0.5 - tv).clamp_min(0.0)).sum() / denom)
            opt.zero_grad(set_to_none=True)
            loss.backward()
            torch.nn.utils.clip_grad_norm_(net.parameters(), 1.0)
            opt.step()
            sched.step()
            tot += loss.detach(); nb += 1
        print(f"ep {ep + 1}/{a.epochs} "
              f"loss {tot.item() / max(1, nb):.4f}", flush=True)
        torch.save({"net": net.state_dict(), "opt": opt.state_dict(),
                    "sched": sched.state_dict(), "epoch": ep}, ckpt_path)

    torch.save(net.state_dict(), os.path.join(a.out, "flow.pt"))

    # ---- export: sampler-unrolled ONNX + vocab json ----
    net_cpu = FlowDiT(V, T, dim=a.dim, layers=a.layers)
    net_cpu.load_state_dict({k: v.cpu() for k, v in net.state_dict().items()})
    net_cpu.eval()
    samp = Sampler(net_cpu, V, T, a.steps, a.guidance).eval()
    Z = T * C6
    tokens = torch.zeros(1, V); tokens[0, 0] = 1.0
    seed = torch.randn(1, Z) * 0.5
    onnx_path = os.path.join(a.out, "t2m.onnx")
    torch.onnx.export(samp, (tokens, seed), onnx_path,
                      input_names=["tokens", "seed"],
                      output_names=["motion"], opset_version=17,
                      dynamo=False)
    vj = {
        "vocab": vocab, "Z": Z, "T": T, "C": J * 10, "J": J,
        "fps": fps, "frame": "world", "version": "v5-flow",
        "flowSteps": a.steps,
        # #858: the canonical reference triple — model clips ride the same
        # bind-referenced direction retarget as v5 template clips.
        "restWorld": [[0.0, 0.0, 0.0, 1.0]] * J,
        "restDir": [[float(v) for v in row] for row in canon_rd],
    }
    with open(os.path.join(a.out, "t2m-vocab.json"), "w") as f:
        json.dump(vj, f)
    print(f"exported {onnx_path} "
          f"({os.path.getsize(onnx_path) / 1e6:.1f} MB) + vocab")

    # sanity: run one sample through onnxruntime
    try:
        import onnxruntime as ort
        s = ort.InferenceSession(onnx_path,
                                 providers=["CPUExecutionProvider"])
        out = s.run(None, {"tokens": tokens.numpy(),
                           "seed": seed.numpy()})[0]
        q = out[0, :, 3:7]
        nrm = np.linalg.norm(out[0].reshape(T, J, 10)[..., 3:7], axis=-1)
        print("onnx ok:", out.shape, "quat norms",
              nrm.min().round(4), nrm.max().round(4))
    except Exception as e:  # noqa: BLE001
        print("onnx check failed:", e)


if __name__ == "__main__":
    main()
