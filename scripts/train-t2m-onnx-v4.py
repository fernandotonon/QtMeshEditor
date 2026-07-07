#!/usr/bin/env python3
# ruff: noqa: E702, E741
"""Train + export the v4 text-to-motion model to ONNX (#411).

ONE-TIME, OFFLINE developer tool — NOT shipped; the app never runs Python. The
app runs the resulting t2m.onnx via ONNX Runtime and retargets the canonical
22-joint output onto the user rig (AnimationMerger::applyMotionClip).

WHY v4 (the v3 model produced UNUSABLE motion — the character folded forward
and flailed at ~3.5× real angular velocity):
  1. v3's delta-head + cumsum integration ACCUMULATED per-frame errors into
     large drifts (the fold). v4 predicts ABSOLUTE per-frame poses from a
     transformer decoder whose frames self-attend (temporal coherence is
     learned, not enforced by integration).
  2. v3 trained on RAW-120fps 40-frame windows (0.33 s — a third of a walk
     cycle) but the app plays clips at 30 fps. v4 trains on the
     prep-t2m-v4.py cache: 30 fps, 60-frame (2 s) windows — playback-exact.
  3. v3 trained on LOCAL quats consumed by the weaker v1 retarget path. v4
     trains on WORLD-frame FK quats with NEUTRAL-start windows, so the output
     rides the same world-frame retarget as the template library
     (vocab json carries "frame":"world").

Contract (superset of v3 — the C++ side reads T/C/vocab from the json):
  input  "tokens" float32 [1, V]    bag-of-action-words over VOCAB
  input  "seed"   float32 [1, Z]    latent (zeros → per-action mean motion)
  output "motion" float32 [1, T, C] C = 22*10 [tx,ty,tz, qx,qy,qz,qw, sx,sy,sz]
  vocab json: {vocab, Z, T, C, J, joints, fps, frame:"world"}

Usage:
  python prep-t2m-v4.py --bvh <dir> --index <txt> --out /tmp/t2m_v4.npz
  python train-t2m-onnx-v4.py --cache /tmp/t2m_v4.npz --out t2m.onnx --epochs 300
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
J = len(CANON)
C = J * 10
Z = 24


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cache", default="/tmp/t2m_v4.npz")
    ap.add_argument("--out", default="t2m.onnx")
    ap.add_argument("--epochs", type=int, default=300)
    ap.add_argument("--batch", type=int, default=128)
    ap.add_argument("--lr", type=float, default=3e-4)
    ap.add_argument("--device", default="auto", choices=["auto", "cpu", "mps"])
    ap.add_argument("--quick", action="store_true")
    ap.add_argument("--z0-target", default="mean", choices=["mean", "medoid"],
                    help="what z=0 reconstructs: each sample (mean — smooth, "
                         "gentle, upright; SHIPPED) or the action's medoid "
                         "exemplar (crisper velocity stats but the imperfect "
                         "imitation of a dynamic real clip renders twisted)")
    a = ap.parse_args()

    import torch
    import torch.nn as nn
    import torch.nn.functional as F

    d = np.load(a.cache, allow_pickle=False)
    mo, tk = d["mo"], d["tk"]                  # mo [N,T,J,4] WORLD quats
    VOCAB = [str(v) for v in d["vocab"]]
    FPS = int(d["fps"])

    # MIRROR AUGMENTATION: reflect across the sagittal plane (x → −x) and swap
    # left/right joint channels. For a world rotation stored (x,y,z,w) the
    # reflected rotation is (x,−y,−z,w). Doubles the data and removes L/R bias
    # (rare actions have single-digit source trials).
    SWAP = list(range(len(CANON)))
    for r0, l0, n in ((6, 10, 4), (14, 18, 4)):     # arms, legs
        for k in range(n):
            SWAP[r0 + k], SWAP[l0 + k] = l0 + k, r0 + k
    mo_m = mo[:, :, SWAP].copy()
    mo_m[..., 1] *= -1.0; mo_m[..., 2] *= -1.0
    mo = np.concatenate([mo, mo_m]); tk = np.concatenate([tk, tk])

    N, T = mo.shape[0], mo.shape[1]
    V = tk.shape[1]
    if a.quick:
        sub = np.random.RandomState(0).permutation(N)[:4000]
        mo, tk = mo[sub], tk[sub]; N = mo.shape[0]
        a.epochs = min(a.epochs, 30)
    dev = a.device
    if dev == "auto":
        dev = "mps" if torch.backends.mps.is_available() else "cpu"
    print(f"data N={N} T={T} J={J} V={V} fps={FPS} dev={dev} epochs={a.epochs}")

    # ---- rotation representations (6D, Zhou et al. 2019) ---------------------
    def quat_to_mat(q):
        x, y, z, w = q[..., 0], q[..., 1], q[..., 2], q[..., 3]
        n = (q * q).sum(-1).clamp_min(1e-8); s = 2.0 / n
        xx, yy, zz = x*x*s, y*y*s, z*z*s
        xy, xz, yz = x*y*s, x*z*s, y*z*s
        wx, wy, wz = w*x*s, w*y*s, w*z*s
        m = torch.stack([1-(yy+zz), xy-wz, xz+wy,
                         xy+wz, 1-(xx+zz), yz-wx,
                         xz-wy, yz+wx, 1-(xx+yy)], -1)
        return m.reshape(q.shape[:-1] + (3, 3))

    def mat_to_6d(m):
        # 6D = the first two COLUMNS of the rotation matrix, concatenated as
        # [col0; col1] (Zhou et al. 2019). m[..., :, :2] is a 3x2 slice whose
        # row-major flatten would INTERLEAVE the columns ([m00,m01,m10,...] —
        # identity -> [1,0,0,1,0,0], two parallel vectors -> degenerate). Move
        # the column axis before flattening so we get [col0(3); col1(3)].
        return m[..., :, :2].transpose(-1, -2).reshape(m.shape[:-2] + (6,))

    def sixd_to_mat(d6):
        a1 = d6[..., 0:3]; a2 = d6[..., 3:6]
        b1 = F.normalize(a1, dim=-1)
        a2 = a2 - (b1 * a2).sum(-1, keepdim=True) * b1
        b2 = F.normalize(a2, dim=-1)
        b3 = torch.cross(b1, b2, dim=-1)
        return torch.stack([b1, b2, b3], dim=-1)

    def mat_to_quat(m):
        t = m[..., 0, 0] + m[..., 1, 1] + m[..., 2, 2]
        w = torch.sqrt(torch.clamp(1 + t, min=1e-8)) / 2
        w4 = (4 * w).clamp_min(1e-8)
        x = (m[..., 2, 1] - m[..., 1, 2]) / w4
        y = (m[..., 0, 2] - m[..., 2, 0]) / w4
        z = (m[..., 1, 0] - m[..., 0, 1]) / w4
        return torch.stack([x, y, z, w], -1)

    Q = torch.tensor(mo, dtype=torch.float32)
    R6 = mat_to_6d(quat_to_mat(Q)).reshape(N, T, J * 6)
    Tk = torch.tensor(tk, dtype=torch.float32)
    D6 = J * 6

    # ---- model: CVAE, transformer decoder with self+cross attention ----------
    class PosEnc(nn.Module):
        def __init__(s, dim, n):
            super().__init__(); s.pe = nn.Parameter(torch.randn(n, dim) * 0.02)
        def forward(s, x): return x + s.pe.unsqueeze(0)[:, :x.shape[1]]

    class DecLayer(nn.Module):
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
            s.einp = nn.Linear(D6, dim); s.epe = PosEnc(dim, T)
            el = nn.TransformerEncoderLayer(dim, heads, dim*4,
                                            batch_first=True, activation="gelu")
            s.enc = nn.TransformerEncoder(el, layers)
            s.mu = nn.Linear(dim, Z); s.lv = nn.Linear(dim, Z)
            s.cond = nn.Sequential(nn.Linear(V + Z, dim), nn.GELU(),
                                   nn.Linear(dim, 2 * dim))   # 2 condition tokens
            s.q = nn.Parameter(torch.randn(T, dim) * 0.02)
            s.dec = nn.ModuleList([DecLayer(dim, heads) for _ in range(layers)])
            # ABSOLUTE pose head (v4): frames already self-attend every layer,
            # so temporal coherence is modelled — no error-accumulating cumsum.
            s.out = nn.Linear(dim, D6)
        def encode(s, m):
            h = s.enc(s.epe(s.einp(m)))
            g = h.mean(1)
            return s.mu(g), s.lv(g)
        def decode(s, tokens, z):
            B = tokens.shape[0]
            cond = s.cond(torch.cat([tokens, z], -1)).reshape(B, 2, -1)
            x = s.q.unsqueeze(0).expand(B, T, -1)
            for layer in s.dec:
                x = layer(x, cond)
            return s.out(x)
        def forward(s, tokens, z):
            return s.decode(tokens, z)

    net = T2M().to(dev)
    print(f"model params: {sum(p.numel() for p in net.parameters())/1e6:.2f}M")
    opt = torch.optim.AdamW(net.parameters(), lr=a.lr, weight_decay=1e-5)
    sch = torch.optim.lr_scheduler.CosineAnnealingLR(opt, a.epochs)
    bs = a.batch

    def recon_loss_ps(pred6, tgt6):
        """Per-sample recon [B] (6D L2 + orthogonality nudge)."""
        l2 = ((pred6 - tgt6) ** 2).mean(dim=(1, 2))
        a1 = pred6.reshape(pred6.shape[0], -1, J, 6)[..., 0:3]
        a2 = pred6.reshape(pred6.shape[0], -1, J, 6)[..., 3:6]
        orth = (F.normalize(a1, dim=-1) * F.normalize(a2, dim=-1)) \
            .sum(-1).abs().mean(dim=(1, 2))
        return l2 + 0.05 * orth

    def vel_ps(x):   # per-sample velocity magnitude [B]
        return (x[:, 1:] - x[:, :-1]).abs().mean(dim=(1, 2))
    def accel_ps(x):
        dd = x[:, 1:] - x[:, :-1]
        return (dd[:, 1:] - dd[:, :-1]).abs().mean(dim=(1, 2))

    # Canonical parent map (matches AnimationMerger's kParentCanon): the
    # retarget consumes LOCAL rotations derived as parentᵀ·child, so per-joint
    # world errors along the 5-link spine chain each become a visible local
    # bend (the "fold"). We therefore supervise the DERIVED LOCALS directly.
    PARENT = [-1, 0, 1, 2, 3, 4,  2, 6, 7, 8,  2, 10, 11, 12,
              0, 14, 15, 16,  0, 18, 19, 20]
    chi = torch.tensor([c for c in range(J) if PARENT[c] >= 0], device=dev)
    par = torch.tensor([PARENT[c] for c in range(J) if PARENT[c] >= 0], device=dev)

    def derived_locals(x6):
        """[B,T,J,6] world 6D → local rotation matrices [B,T,21,3,3]."""
        m = sixd_to_mat(x6.reshape(x6.shape[0], -1, J, 6))
        mp = m[:, :, par]; mc = m[:, :, chi]
        return mp.transpose(-1, -2) @ mc

    def mat_angle(a, b):
        tr = (a * b).sum(dim=(-1, -2))
        return torch.arccos(((tr - 1) / 2).clamp(-1 + 1e-6, 1 - 1e-6))

    def geo_vel_ps(x6):
        """Per-sample ROTATION-space (geodesic) frame-to-frame velocity [B].
        6D L1 velocity under-measures jitter (small 6D deltas can be large
        rotations), so the magnitude match must happen on actual angles."""
        m = sixd_to_mat(x6.reshape(x6.shape[0], T, J, 6))       # [B,T,J,3,3]
        a, b = m[:, :-1], m[:, 1:]
        tr = (a * b).sum(dim=(-1, -2))                          # tr(aᵀb)
        ang = torch.arccos(((tr - 1) / 2).clamp(-1 + 1e-6, 1 - 1e-6))
        return ang.mean(dim=(1, 2))

    # Per-action MEDOID exemplar: the real window closest to the action's mean
    # in 6D space. z=0 (the app's inference latent) is supervised toward THIS
    # instead of each individual sample — a conditional-mean target averages
    # phase-misaligned windows into mush, while the medoid is a crisp real
    # clip. z~posterior keeps per-sample reconstruction (variety).
    act_idx = torch.tensor(tk.argmax(1))
    flat = R6.reshape(N, -1)
    medoid6 = torch.zeros(V, T, D6)
    for aI in range(V):
        m_ = (act_idx == aI).nonzero(as_tuple=True)[0]
        if not len(m_): continue
        mu_a = flat[m_].mean(0, keepdim=True)
        medoid6[aI] = R6[m_[int((flat[m_] - mu_a).pow(2).sum(1).argmin())]]

    # class-balanced sampling (rare actions seen ~as often as walk)
    tk_np = tk.astype(np.float64)
    inv = 1.0 / (tk_np.sum(0) + 1.0)
    w_win = (tk_np * inv[None, :]).max(1)
    sample_p = torch.tensor(w_win / w_win.sum(), dtype=torch.float64)
    steps = (N + bs - 1) // bs

    for ep in range(a.epochs):
        net.train()
        rtot = vtot = 0.0; nb = 0
        beta = min(1.0, ep / max(1, a.epochs * 0.3)) * 2e-4
        idx = torch.multinomial(sample_p, steps * bs, replacement=True)
        for b in range(0, steps * bs, bs):
            bi = idx[b:b + bs]
            tokens = Tk[bi].to(dev); tgt = R6[bi].to(dev)
            mu, lv = net.encode(tgt)
            z = mu + torch.randn_like(mu) * (0.5 * lv).exp()
            # LATENT DROPOUT: the app runs inference with z = 0, which a
            # low-beta CVAE never visits (the v4.0 model matched velocity in
            # training yet flailed at z=0 — out-of-distribution latent). Train
            # ~15% of each batch at exactly z=0 so the inference condition is
            # supervised: magnitude losses at full weight, recon at 0.25 (the
            # z=0 target is the action's TYPICAL clip, not any one sample).
            dropm = (torch.rand(z.shape[0], device=dev) < 0.2)
            z = torch.where(dropm.unsqueeze(1), torch.zeros_like(z), z)
            pred = net.decode(tokens, z)
            # z=0 samples: either reconstruct their own window at reduced
            # weight (mean target — smooth/upright, the shipped default) or
            # the action's medoid exemplar at full weight (--z0-target medoid).
            if a.z0_target == "medoid":
                tgt = torch.where(dropm.view(-1, 1, 1),
                                  medoid6[act_idx[bi]].to(dev), tgt)
                recon = recon_loss_ps(pred, tgt).mean()
            else:
                rw = torch.where(dropm, torch.full_like(z[:, 0], 0.25),
                                 torch.ones_like(z[:, 0]))
                recon = (recon_loss_ps(pred, tgt) * rw).mean()
            kl = (-0.5 * (1 + lv - mu*mu - lv.exp())).mean()
            # AGGREGATE-POSTERIOR moment match: pull the latent cloud toward
            # N(0,1) so z=0 sits at its centre (KL alone is too weak at this
            # beta). Mean → 0, aggregate variance (var(mu)+E[var]) → 1.
            am = mu.mean(0)
            av = mu.var(0, unbiased=False) + lv.exp().mean(0)
            agg = am.pow(2).mean() + (av - 1).abs().mean()
            # PER-SAMPLE magnitude matching (a global mean would let idle come
            # out fast and run slow as long as the average matched), in BOTH
            # 6D and true rotation space (6D L1 under-measures jitter).
            vmatch = (vel_ps(pred) - vel_ps(tgt)).abs().mean()
            amatch = (accel_ps(pred) - accel_ps(tgt)).abs().mean()
            gmatch = (geo_vel_ps(pred) - geo_vel_ps(tgt)).abs().mean()
            # DERIVED-LOCAL supervision — pose (geodesic to target locals) and
            # per-sample local velocity magnitude. This is what the retarget
            # renders; world-only losses let spine-chain errors stack into a
            # visible fold.
            lp = derived_locals(pred); lt = derived_locals(tgt)
            lgeo = mat_angle(lp, lt).mean()
            lv_p = mat_angle(lp[:, 1:], lp[:, :-1]).mean(dim=(1, 2))
            lv_t = mat_angle(lt[:, 1:], lt[:, :-1]).mean(dim=(1, 2))
            lgv = (lv_p - lv_t).abs().mean()
            loss = recon + beta * kl + 0.05 * agg \
                   + 1.0 * vmatch + 1.0 * amatch + 2.0 * gmatch \
                   + 3.0 * lgeo + 2.0 * lgv
            opt.zero_grad(); loss.backward()
            torch.nn.utils.clip_grad_norm_(net.parameters(), 1.0)
            opt.step()
            rtot += recon.item(); vtot += geo_vel_ps(pred).mean().item(); nb += 1
        sch.step()
        if ep % 5 == 0 or ep == a.epochs - 1:
            with torch.no_grad():
                rv = float(geo_vel_ps(R6[:256].to(dev)).mean())
            print(f"ep{ep:3d} recon{rtot/max(1,nb):.5f} geovel{vtot/max(1,nb):.4f}rad "
                  f"(real~{rv:.4f}) beta{beta:.1e}", flush=True)

    # ---- eval: per-action angular velocity vs real + distinctness ------------
    net.eval().cpu()
    with torch.no_grad():
        def gen6(word):
            t = torch.zeros(1, V)
            if word in VOCAB: t[0, VOCAB.index(word)] = 1.0
            return net(t, torch.zeros(1, Z))
        outs = {w: gen6(w) for w in VOCAB}
        def angvel_deg(x6):
            m = sixd_to_mat(x6.reshape(1, T, J, 6))
            q = mat_to_quat(m)[0]
            dots = (q[1:] * q[:-1]).sum(-1).abs().clamp(0, 1)
            return float(torch.rad2deg(2 * torch.arccos(dots)).mean())
        for w in VOCAB:
            m_ = tk[:, VOCAB.index(w)] > 0
            if not m_.any(): continue
            real_q = torch.tensor(mo[m_][:64], dtype=torch.float32)
            dots = (real_q[:, 1:] * real_q[:, :-1]).sum(-1).abs().clamp(0, 1)
            rv = float(torch.rad2deg(2 * torch.arccos(dots)).mean())
            print(f"  {w:7s} gen angvel {angvel_deg(outs[w]):5.2f}°/f  real {rv:5.2f}°/f")
        pairs = [("walk", "wave"), ("walk", "run"), ("dance", "sit")]
        for x, y in pairs:
            print(f"  |{x}-{y}| = {float((outs[x]-outs[y]).abs().mean()):.4f}")

    # ---- export ---------------------------------------------------------------
    class Wrap(nn.Module):
        def __init__(s, g): super().__init__(); s.g = g
        def forward(s, tokens, seed):
            d6 = s.g(tokens, seed)
            # 1-2-1 temporal smoothing (kills residual frame-to-frame jitter;
            # ONNX-friendly, endpoints kept)
            mid = 0.25 * d6[:, :-2] + 0.5 * d6[:, 1:-1] + 0.25 * d6[:, 2:]
            d6 = torch.cat([d6[:, :1], mid, d6[:, -1:]], dim=1)
            B = tokens.shape[0]
            q = mat_to_quat(sixd_to_mat(d6.reshape(B, T, J, 6)))
            out = torch.zeros(B, T, J, 10)
            out[..., 3:7] = q
            out[..., 7:10] = 1.0
            return out.reshape(B, T, C)
    dummy = (torch.zeros(1, V), torch.zeros(1, Z))
    torch.onnx.export(Wrap(net).eval(), dummy, a.out,
                      input_names=["tokens", "seed"], output_names=["motion"],
                      dynamic_axes={"motion": {0: "B"}}, opset_version=17,
                      dynamo=False)
    with open(os.path.splitext(a.out)[0] + "-vocab.json", "w") as f:
        json.dump({"vocab": VOCAB, "Z": Z, "T": T, "C": C, "J": J,
                   "joints": CANON, "fps": FPS, "frame": "world"}, f)
    print("wrote", a.out, "+ vocab json (frame=world)")


if __name__ == "__main__":
    main()
