#!/usr/bin/env python3
# ruff: noqa: E702, E741
"""Train + export a text-to-motion model to ONNX (#411) — improved architecture.

ONE-TIME, OFFLINE developer tool — NOT shipped; the app never runs Python. The
app runs the resulting t2m.onnx in C++ via ONNX Runtime, retargeting the
canonical 22-joint output onto the user rig via the #409 adapter (with the
template-clip retarget as the runtime fallback).

WHY A REWRITE (vs scripts/export-t2m-onnx.py, the spike prototype that COLLAPSED
to a static mean pose): the prototype learned in raw QUATERNION space (hard:
double-cover + unit-norm constraint) and conditioned the decoder with a single
broadcast vector, so the loss-optimal output was the dataset mean. Fixes here:
  1. 6D ROTATION representation (Zhou et al. 2019) — continuous, no double cover,
     no normalisation constraint → the network actually converges on rotations.
  2. CROSS-ATTENTION decoder: learned per-frame queries attend to the
     (action ⊕ latent) condition every layer, so frames genuinely differ and the
     action label actually steers the motion.
  3. CVAE (encoder seen only in training) so the latent carries the SPECIFIC clip
     (reconstruction pressure), + KL warmup, + VELOCITY-matching loss + a
     per-frame VARIANCE floor (both anti-collapse: a static clip is penalised).

DATA: the cached preprocessing from the prototype (/tmp/t2m_preprocessed.npz:
mo[N,T,22,4] quats + tk[N,16] bag-of-words) — CMU MoCap (commercial-OK, the same
basis as #409 RMIB). AMASS/HumanML3D/KIT are non-commercial and excluded.

CONTRACT (unchanged from the prototype — the C++ side already targets it):
  input  "tokens" float32 [1, V]    bag-of-action-words over the fixed vocab
  input  "seed"   float32 [1, Z]    latent noise for sample variety
  output "motion" float32 [1, T, C] per-frame canonical pose; C = 22*10 = 220
  (channels per joint: [tx,ty,tz, qx,qy,qz,qw, sx,sy,sz]; we predict rotation,
   write t=0/scale=1 — same as the template clips, so the retarget is identical.)

Usage:
  python train-t2m-onnx.py --cache /tmp/t2m_preprocessed.npz --out t2m.onnx \
      --epochs 300 --batch 256
"""
import argparse
import json
import os

import numpy as np


CANON = ["hip", "abdomen", "chest", "neck", "neck1", "head",
         "rcollar", "rshoulder", "relbow", "rhand",
         "lcollar", "lshoulder", "lelbow", "lhand",
         "rbuttock", "rhip", "rknee", "rfoot",
         "lbuttock", "lhip", "lknee", "lfoot"]
VOCAB = ["walk", "run", "jog", "jump", "dance", "march", "climb", "kick",
         "punch", "sit", "stretch", "throw", "wave", "boxing", "turn", "forward"]
J = len(CANON)
C = J * 10
Z = 24            # latent seed dim (a touch larger than the prototype's 16)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cache", default="/tmp/t2m_preprocessed.npz")
    ap.add_argument("--out", default="t2m.onnx")
    ap.add_argument("--epochs", type=int, default=300)
    ap.add_argument("--batch", type=int, default=256)
    ap.add_argument("--lr", type=float, default=3e-4)
    ap.add_argument("--quick", action="store_true",
                    help="tiny run (few epochs, subset) to sanity-check learning")
    ap.add_argument("--device", default="auto", choices=["auto", "cpu", "mps"])
    a = ap.parse_args()

    import torch
    import torch.nn as nn
    import torch.nn.functional as F

    d = np.load(a.cache)
    mo, tk = d["mo"], d["tk"]              # mo [N,T,J,4] quats, tk [N,V]
    N, T = mo.shape[0], mo.shape[1]
    V = tk.shape[1]
    if a.quick:
        sub = np.random.RandomState(0).permutation(N)[:8000]
        mo, tk = mo[sub], tk[sub]; N = mo.shape[0]
        a.epochs = min(a.epochs, 30)
    # Device: MPS is much faster than CPU for this transformer once kernels are
    # compiled (first epoch is slow, then ~5-10x). Allow override via --device.
    dev = a.device
    if dev == "auto":
        dev = "mps" if torch.backends.mps.is_available() else "cpu"
    print(f"data N={N} T={T} J={J} V={V} Z={Z} dev={dev} epochs={a.epochs}")

    # ---- rotation representations -------------------------------------------
    # quaternion (x,y,z,w) -> rotation matrix -> 6D (first two columns).
    def quat_to_mat(q):                    # [...,4] -> [...,3,3]
        x, y, z, w = q[..., 0], q[..., 1], q[..., 2], q[..., 3]
        n = (q * q).sum(-1).clamp_min(1e-8)
        s = 2.0 / n
        xx, yy, zz = x*x*s, y*y*s, z*z*s
        xy, xz, yz = x*y*s, x*z*s, y*z*s
        wx, wy, wz = w*x*s, w*y*s, w*z*s
        m = torch.stack([
            1-(yy+zz), xy-wz,     xz+wy,
            xy+wz,     1-(xx+zz), yz-wx,
            xz-wy,     yz+wx,     1-(xx+yy)], -1)
        return m.reshape(q.shape[:-1] + (3, 3))

    def mat_to_6d(m):                      # [...,3,3] -> [...,6] (first 2 cols)
        return m[..., :, :2].reshape(m.shape[:-2] + (6,))

    def sixd_to_mat(d6):                   # [...,6] -> [...,3,3] (Gram-Schmidt)
        a1 = d6[..., 0:3]; a2 = d6[..., 3:6]
        b1 = F.normalize(a1, dim=-1)
        a2 = a2 - (b1 * a2).sum(-1, keepdim=True) * b1
        b2 = F.normalize(a2, dim=-1)
        b3 = torch.cross(b1, b2, dim=-1)
        return torch.stack([b1, b2, b3], dim=-1)

    def mat_to_quat(m):                    # [...,3,3] -> [...,4] (x,y,z,w)
        # robust branchless-ish; good enough for export.
        t = m[..., 0, 0] + m[..., 1, 1] + m[..., 2, 2]
        w = torch.sqrt(torch.clamp(1 + t, min=1e-8)) / 2
        w4 = (4 * w).clamp_min(1e-8)
        x = (m[..., 2, 1] - m[..., 1, 2]) / w4
        y = (m[..., 0, 2] - m[..., 2, 0]) / w4
        z = (m[..., 1, 0] - m[..., 0, 1]) / w4
        return torch.stack([x, y, z, w], -1)

    # Precompute the dataset in 6D: [N,T,J,6] -> flatten [N,T,J*6]
    Q = torch.tensor(mo, dtype=torch.float32)               # [N,T,J,4]
    R6 = mat_to_6d(quat_to_mat(Q)).reshape(N, T, J * 6)     # [N,T,J*6]
    Tk = torch.tensor(tk, dtype=torch.float32)              # [N,V]
    D6 = J * 6
    print(f"6D motion tensor: {tuple(R6.shape)}")

    # ---- model ---------------------------------------------------------------
    class PosEnc(nn.Module):
        def __init__(s, dim, n):
            super().__init__(); s.pe = nn.Parameter(torch.randn(n, dim) * 0.02)
        def forward(s, x): return x + s.pe.unsqueeze(0)[:, :x.shape[1]]

    class CrossDecoderLayer(nn.Module):
        """Self-attn over frames + cross-attn onto the condition + FFN."""
        def __init__(s, dim, heads):
            super().__init__()
            s.sa = nn.MultiheadAttention(dim, heads, batch_first=True)
            s.ca = nn.MultiheadAttention(dim, heads, batch_first=True)
            s.ff = nn.Sequential(nn.Linear(dim, dim*4), nn.GELU(), nn.Linear(dim*4, dim))
            s.n1 = nn.LayerNorm(dim); s.n2 = nn.LayerNorm(dim); s.n3 = nn.LayerNorm(dim)
        def forward(s, x, cond):
            x = x + s.sa(s.n1(x), s.n1(x), s.n1(x))[0]
            xn = s.n2(x)
            x = x + s.ca(xn, cond, cond)[0]
            x = x + s.ff(s.n3(x))
            return x

    class T2M(nn.Module):
        def __init__(s, dim=256, layers=4, heads=8):
            super().__init__()
            # encoder (training only): real motion -> latent (mu, logvar)
            s.einp = nn.Linear(D6, dim)
            s.epe = PosEnc(dim, T)
            el = nn.TransformerEncoderLayer(dim, heads, dim*4, batch_first=True, activation="gelu")
            s.enc = nn.TransformerEncoder(el, layers)
            s.mu = nn.Linear(dim, Z); s.lv = nn.Linear(dim, Z)
            # condition: action tokens + latent -> a few condition tokens
            s.cond = nn.Sequential(nn.Linear(V + Z, dim), nn.GELU(), nn.Linear(dim, dim))
            # decoder: learned per-frame queries cross-attend to the condition
            s.q = nn.Parameter(torch.randn(T, dim) * 0.02)
            s.dec = nn.ModuleList([CrossDecoderLayer(dim, heads) for _ in range(layers)])
            s.out = nn.Linear(dim, D6)
        def encode(s, m):
            h = s.enc(s.epe(s.einp(m)))
            g = h.mean(1)
            return s.mu(g), s.lv(g)
        def decode(s, tokens, z):
            B = tokens.shape[0]
            cond = s.cond(torch.cat([tokens, z], -1)).unsqueeze(1)   # [B,1,dim]
            x = s.q.unsqueeze(0).expand(B, T, -1)                    # [B,T,dim]
            for layer in s.dec:
                x = layer(x, cond)
            return s.out(x)                                          # [B,T,D6]
        def forward(s, tokens, z):
            return s.decode(tokens, z)

    net = T2M().to(dev)
    nparams = sum(p.numel() for p in net.parameters())
    print(f"model params: {nparams/1e6:.2f}M")
    opt = torch.optim.AdamW(net.parameters(), lr=a.lr, weight_decay=1e-5)
    sch = torch.optim.lr_scheduler.CosineAnnealingLR(opt, a.epochs)
    bs = a.batch

    def recon_loss(pred6, tgt6):
        # 6D L2 + a Gram-Schmidt-normalised term. The plain 6D L2 is cheap (no
        # matrix build per batch — the earlier matrix-Frobenius loss dominated CPU
        # time) and is a valid smooth rotation objective once the target 6D is the
        # canonical first-two-columns form (it is). The normalised b1·b2 term just
        # nudges the two predicted basis vectors toward orthonormal so decode is
        # stable; full matrix metric is used only for the occasional eval print.
        l2 = ((pred6 - tgt6) ** 2).mean()
        a1 = pred6.reshape(-1, J, 6)[..., 0:3]
        a2 = pred6.reshape(-1, J, 6)[..., 3:6]
        orth = (F.normalize(a1, dim=-1) * F.normalize(a2, dim=-1)).sum(-1).abs().mean()
        return l2 + 0.05 * orth

    def vel(x6):                            # frame-to-frame 6D change magnitude
        return (x6[:, 1:] - x6[:, :-1]).abs().mean()

    def variance(x6):                       # per-(joint,channel) temporal variance
        return x6.var(dim=1).mean()

    # CLASS-BALANCED SAMPLING. The CMU label set is severely imbalanced — `walk`
    # is 64% of windows, `wave`/`march` ~1.4%. Uniform sampling makes the loss-
    # optimal model output "walk-like" motion for every prompt (the low action
    # distinctness we measured). Weight each window by the inverse frequency of
    # its RAREST action word so rare actions are seen ~as often as walk; sample
    # an epoch's worth of indices WITH replacement from those weights.
    tk_np = tk.astype(np.float64)
    action_freq = tk_np.sum(0) + 1.0                       # [V]
    inv = 1.0 / action_freq
    # per-window weight = max inverse-freq over the action words it carries
    w_win = (tk_np * inv[None, :]).max(1)
    w_win = np.where(w_win > 0, w_win, inv.min())          # safety for empties
    sample_p = torch.tensor(w_win / w_win.sum(), dtype=torch.float64)
    steps_per_epoch = (N + bs - 1) // bs

    for ep in range(a.epochs):
        net.train()
        rtot = vtot = 0.0; nb = 0
        beta = min(1.0, ep / max(1, a.epochs * 0.3)) * 2e-4   # KL warmup
        # draw a balanced index stream for this epoch (with replacement)
        idx_stream = torch.multinomial(sample_p, steps_per_epoch * bs, replacement=True)
        for b in range(0, steps_per_epoch * bs, bs):
            bi = idx_stream[b:b+bs]
            tokens = Tk[bi].to(dev); tgt = R6[bi].to(dev)
            mu, lv = net.encode(tgt)
            z = mu + torch.randn_like(mu) * (0.5 * lv).exp()
            pred = net.decode(tokens, z)
            recon = recon_loss(pred, tgt)
            kl = (-0.5 * (1 + lv - mu*mu - lv.exp())).mean()
            # anti-collapse: match the target's motion magnitude AND keep variance
            vmatch = (vel(pred) - vel(tgt)).abs()
            vfloor = F.relu(0.5 * variance(tgt) - variance(pred))   # penalise < half target var
            loss = recon + beta * kl + 0.3 * vmatch + 0.5 * vfloor
            opt.zero_grad(); loss.backward()
            torch.nn.utils.clip_grad_norm_(net.parameters(), 1.0)
            opt.step()
            rtot += recon.item(); vtot += vel(pred).item(); nb += 1
        sch.step()
        if ep % 5 == 0 or ep == a.epochs - 1:
            with torch.no_grad():
                rv = float(vel(R6[:512]))
            print(f"ep{ep:3d} recon{rtot/max(1,nb):.5f} genvel{vtot/max(1,nb):.4f} "
                  f"(real~{rv:.4f}) beta{beta:.1e}", flush=True)

    # ---- sanity: do different actions yield different motion? ----------------
    net.eval().cpu()   # move off MPS so the eval/export inputs (CPU) match
    with torch.no_grad():
        def gen(word):
            t = torch.zeros(1, V)
            if word in VOCAB: t[0, VOCAB.index(word)] = 1.0
            return net(t, torch.zeros(1, Z))
        walk, run, wave = gen("walk"), gen("run"), gen("wave")
        dwr = float((walk - run).abs().mean())
        dww = float((walk - wave).abs().mean())
        print(f"action distinctness: |walk-run|={dwr:.4f} |walk-wave|={dww:.4f} "
              f"(want >> 0; ~0 = collapsed)")

    # ---- export: wrap so output is the [1,T,C=220] quaternion contract --------
    class Wrap(nn.Module):
        def __init__(s, g): super().__init__(); s.g = g
        def forward(s, tokens, seed):
            d6 = s.g(tokens, seed)                       # [B,T,D6]
            B = tokens.shape[0]
            m = sixd_to_mat(d6.reshape(B, T, J, 6))      # [B,T,J,3,3]
            q = mat_to_quat(m)                           # [B,T,J,4]
            out = torch.zeros(B, T, J, 10)
            out[..., 3:7] = q
            out[..., 7:10] = 1.0
            return out.reshape(B, T, C)
    net.cpu()
    dummy = (torch.zeros(1, V), torch.zeros(1, Z))
    torch.onnx.export(
        Wrap(net).eval(), dummy, a.out,
        input_names=["tokens", "seed"], output_names=["motion"],
        dynamic_axes={"motion": {0: "B"}}, opset_version=17, dynamo=False)
    with open(os.path.splitext(a.out)[0] + "-vocab.json", "w") as f:
        json.dump({"vocab": VOCAB, "Z": Z, "T": T, "C": C, "J": J,
                   "joints": CANON}, f)
    print("wrote", a.out, "+ vocab json")


if __name__ == "__main__":
    main()
