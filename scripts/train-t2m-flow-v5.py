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
DIR_CANON = [
    [0, 1, 0], [0, 1, 0], [0, 1, 0], [0, 1, 0], [0, 1, 0], [0, 1, 0],
    [-1, 0, 0], [-1, 0, 0], [-1, 0, 0], [-1, 0, 0],
    [1, 0, 0], [1, 0, 0], [1, 0, 0], [1, 0, 0],
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
                loss = loss + a.phase_weight * (
                    (g * (0.6 - ph).clamp_min(0.0)).sum() / denom)
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
