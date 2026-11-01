#!/usr/bin/env python3
# ruff: noqa: E702, E741
"""Train the v9 TEXT-conditioned flow-matching text-to-motion model (dev tool, not shipped).

Successor of train-t2m-flow-v5.py (v8): the same DiT-style rectified-flow transformer over 22 canonical
joints x 6D rotation, but conditioned on a Qwen3-Embedding-0.6B sentence vector instead of a one-hot
action, and on the clip LENGTH (samples are padded to Tmax; attention and loss ignore the padding).

Data: prep-t2m-text.py output. Each step picks one of the sample's P text paraphrases; 10% of the texts
are dropped (zero vector) for classifier-free guidance. Sampling is balanced per base prompt and split
--kimodo-share / rest between the Kimodo corpus and the v8 action windows.

Export: ONNX with the Euler sampler unrolled (--steps):
  inputs  text[1,1024] (L2-normalised Qwen3 embedding), seed[1,Tmax*132] ~ N(0,0.5), length[1] frames
  output  motion[1,Tmax,220] (per joint: t=0, quat x,y,z,w, s=1); frames >= length are padding.
plus t2m-text.json (Tmax, fps, encoder, instruction, canonical reference triple).

    ~/kimodo_eval/venv/bin/python scripts/train-t2m-text.py --data ~/t2m_v90/t2m_v90.npz \
        --out ~/t2m_v90/run1 --epochs 200 --device mps
"""
import argparse
import importlib.util
import json
import math
import os
import re
import sys

import numpy as np
import torch
import torch.nn as nn
import torch.nn.functional as F

HERE = os.path.dirname(os.path.abspath(__file__))
_spec = importlib.util.spec_from_file_location("flow5", os.path.join(HERE, "train-t2m-flow-v5.py"))
flow5 = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(flow5)
quat_to_6d, d6_to_quat = flow5.quat_to_6d, flow5.d6_to_quat

_espec = importlib.util.spec_from_file_location("embedt2m", os.path.join(HERE, "embed-t2m-text.py"))
embedt2m = importlib.util.module_from_spec(_espec)
_espec.loader.exec_module(embedt2m)

J, D6 = 22, 6
C6 = J * D6
E = 1024


class Block(nn.Module):
    """AdaLN-Zero transformer block with explicit (export-safe) masked self-attention."""

    def __init__(self, dim, heads):
        super().__init__()
        self.h = heads
        self.n1 = nn.LayerNorm(dim, elementwise_affine=False)
        self.qkv = nn.Linear(dim, dim * 3)
        self.proj = nn.Linear(dim, dim)
        self.n2 = nn.LayerNorm(dim, elementwise_affine=False)
        self.mlp = nn.Sequential(nn.Linear(dim, dim * 4), nn.GELU(), nn.Linear(dim * 4, dim))
        self.ada = nn.Linear(dim, dim * 6)
        nn.init.zeros_(self.ada.weight)
        nn.init.zeros_(self.ada.bias)

    def forward(self, x, c, bias):
        B, T, D = x.shape
        s1, b1, g1, s2, b2, g2 = self.ada(c).unsqueeze(1).chunk(6, -1)
        h = self.n1(x) * (1 + s1) + b1
        q, k, v = self.qkv(h).reshape(B, T, 3, self.h, D // self.h).permute(2, 0, 3, 1, 4)
        att = (q @ k.transpose(-1, -2)) / math.sqrt(D // self.h) + bias   # bias [B,1,1,T]
        h = (att.softmax(-1) @ v).transpose(1, 2).reshape(B, T, D)
        x = x + g1 * self.proj(h)
        h = self.n2(x) * (1 + s2) + b2
        return x + g2 * self.mlp(h)


class TextFlow(nn.Module):
    def __init__(self, T, dim=384, layers=8, heads=8):
        super().__init__()
        self.T = T
        self.inp = nn.Linear(C6, dim)
        self.pos = nn.Parameter(torch.randn(1, T, dim) * 0.02)
        self.txt = nn.Sequential(nn.Linear(E, dim), nn.SiLU(), nn.Linear(dim, dim))
        self.t_mlp = nn.Sequential(nn.Linear(dim, dim), nn.SiLU(), nn.Linear(dim, dim))
        self.l_mlp = nn.Sequential(nn.Linear(dim, dim), nn.SiLU(), nn.Linear(dim, dim))
        self.blocks = nn.ModuleList([Block(dim, heads) for _ in range(layers)])
        self.out_n = nn.LayerNorm(dim, elementwise_affine=False)
        self.out = nn.Linear(dim, C6)
        nn.init.zeros_(self.out.weight)
        nn.init.zeros_(self.out.bias)
        half = dim // 2
        self.register_buffer("freqs", torch.exp(-math.log(1e4) * torch.arange(half).float() / half))
        self.register_buffer("frames", torch.arange(T).float())

    def sincos(self, v):
        ang = v[:, None] * self.freqs[None]
        return torch.cat([torch.sin(ang), torch.cos(ang)], -1)

    def forward(self, x, t, txt, length):
        # x[B,T,C6], t[B] in [0,1], txt[B,E], length[B] frames (float)
        c = self.t_mlp(self.sincos(t * 1000.0)) + self.txt(txt) + self.l_mlp(self.sincos(length))
        valid = (self.frames[None] < length[:, None]).float()          # [B,T]
        bias = ((1.0 - valid) * -1e4)[:, None, None, :]
        h = self.inp(x) + self.pos
        for blk in self.blocks:
            h = blk(h, c, bias)
        return self.out(self.out_n(h))


class Sampler(nn.Module):
    def __init__(self, net, T, steps, guidance, dead=None):
        super().__init__()
        self.net, self.T, self.steps, self.guidance = net, T, steps, guidance
        # Roles no training sample ever supervises (the CMU buttock joints 14/18 are identity in every
        # source): the loss ignores them, so the network emits arbitrary rotations there. Pin them.
        d = torch.zeros(J) if dead is None else torch.as_tensor(dead, dtype=torch.float32)
        self.register_buffer("dead", d[None, None, :, None])

    def forward(self, text, seed, length):
        x = seed.reshape(1, self.T, C6) * 2.0
        uncond = torch.zeros_like(text)
        for i in range(self.steps):
            t = torch.full((1,), i / self.steps, dtype=x.dtype)
            vc = self.net(x, t, text, length)
            vu = self.net(x, t, uncond, length)
            x = x + (vu + self.guidance * (vc - vu)) / self.steps
        q = d6_to_quat(x.reshape(1, self.T, J, D6))
        ident = torch.tensor([0.0, 0.0, 0.0, 1.0], dtype=x.dtype)
        q = q * (1 - self.dead) + ident * self.dead
        z = torch.zeros(1, self.T, J, 3, dtype=x.dtype)
        o = torch.ones(1, self.T, J, 3, dtype=x.dtype)
        return torch.cat([z, q, o], -1).reshape(1, self.T, J * 10)


def random_crop(xb, lb, sb, prob, min_len):
    """Replace some Kimodo samples (src 0) by a random sub-window [o, o+L) of the clip, re-padded by
    repeating its last frame (the same padding prep-t2m-text.py uses)."""
    B, T, _ = xb.shape
    lb = lb.clone()
    pick = (sb == 0) & (torch.rand(B) < prob) & (lb >= min_len + 1)
    if not pick.any():
        return xb, lb
    idx = torch.arange(T)[None].repeat(B, 1)
    for b in torch.nonzero(pick).flatten().tolist():
        n = int(lb[b])
        L = int(torch.randint(min_len, n + 1, ()))
        o = int(torch.randint(0, n - L + 1, ()))
        idx[b] = (o + torch.arange(T)).clamp(max=o + L - 1)
        lb[b] = float(L)
    return torch.gather(xb, 1, idx[:, :, None].expand(-1, -1, xb.shape[2])), lb


def sample_weights(d, kimodo_share, boost=None):
    texts = [str(t) for t in d["texts"]]
    base = [texts[int(i)] for i in d["tv"][:, 0]]
    src = d["src"].astype(int)
    counts = {}
    for b in base:
        counts[b] = counts.get(b, 0) + 1
    w = np.array([1.0 / counts[b] for b in base], np.float64)
    if boost:
        rx, factor = boost
        hit = np.array([bool(rx.search(b)) for b in base])
        w[hit] *= factor
        print(f"boost x{factor}: {int(hit.sum())} samples match {rx.pattern!r}", flush=True)
    for s, share in ((0, kimodo_share), (1, 1.0 - kimodo_share)):
        m = src == s
        if m.any() and w[m].sum() > 0:
            w[m] *= share / w[m].sum()
    return w


def export(net, T, steps, guidance, out, fps, canon_rd, instruct, dead=None):
    net_cpu = TextFlow(T, dim=net.inp.out_features, layers=len(net.blocks), heads=net.blocks[0].h)
    net_cpu.load_state_dict({k: v.cpu() for k, v in net.state_dict().items()})
    net_cpu.eval()
    samp = Sampler(net_cpu, T, steps, guidance, dead).eval()
    text = F.normalize(torch.randn(1, E), dim=-1)
    seed = torch.randn(1, T * C6) * 0.5
    length = torch.tensor([120.0])
    path = os.path.join(out, "t2m-text.onnx")
    with torch.no_grad():
        torch.onnx.export(samp, (text, seed, length), path, input_names=["text", "seed", "length"],
                          output_names=["motion"], opset_version=17, dynamo=False)
    meta = {"version": "v9-text", "T": T, "Z": T * C6, "C": J * 10, "J": J, "fps": fps, "frame": "world",
            "flowSteps": steps, "guidance": guidance, "embedDim": E,
            "encoder": "Qwen3-Embedding-0.6B-Q8_0.gguf", "pooling": "last", "instruct": instruct,
            "restWorld": [[0.0, 0.0, 0.0, 1.0]] * J, "restDir": [[float(v) for v in r] for r in canon_rd]}
    with open(os.path.join(out, "t2m-text.json"), "w") as f:
        json.dump(meta, f, indent=1)
    import onnxruntime as ort
    s = ort.InferenceSession(path, providers=["CPUExecutionProvider"])
    o = s.run(None, {"text": text.numpy(), "seed": seed.numpy(), "length": length.numpy()})[0]
    with torch.no_grad():
        ref = samp(text, seed, length).numpy()
    print(f"exported {path} ({os.path.getsize(path) / 1e6:.1f} MB); onnx vs torch max {np.abs(o - ref).max():.2e}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--data", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--epochs", type=int, default=200)
    ap.add_argument("--batch", type=int, default=128)
    ap.add_argument("--lr", type=float, default=2e-4)
    ap.add_argument("--dim", type=int, default=384)
    ap.add_argument("--layers", type=int, default=8)
    ap.add_argument("--steps", type=int, default=24)
    ap.add_argument("--guidance", type=float, default=2.0)
    ap.add_argument("--kimodo-share", type=float, default=0.7)
    ap.add_argument("--samples-per-epoch", type=int, default=20000)
    ap.add_argument("--device", default="mps")
    ap.add_argument("--resume", action="store_true")
    ap.add_argument("--init", default="", help="start from this checkpoint's weights (fresh optimizer/schedule)")
    ap.add_argument("--vel-weight", type=float, default=0.0,
                    help="weight of a frame-to-frame velocity loss on the predicted clean motion (0 = off)")
    ap.add_argument("--crop-prob", type=float, default=0.0,
                    help="probability of training on a random sub-window of a Kimodo clip instead of the whole "
                         "clip; whole clips always start from rest, so the model otherwise learns a still lead-in")
    ap.add_argument("--crop-min", type=int, default=45, help="shortest random sub-window, frames")
    ap.add_argument("--boost", default="",
                    help="REGEX=FACTOR: multiply the sampling weight of samples whose text matches")
    ap.add_argument("--export-only", action="store_true")
    a = ap.parse_args()

    d = np.load(os.path.expanduser(a.data), allow_pickle=False)
    mo, ln, msk, tv, emb = d["mo"], d["ln"], d["msk"], d["tv"], d["emb"]
    N, T = mo.shape[0], mo.shape[1]
    dev = torch.device(a.device if a.device != "mps" or torch.backends.mps.is_available() else "cpu")
    print(f"data: {N} samples Tmax={T} texts={emb.shape[0]} device={dev}", flush=True)

    x1 = quat_to_6d(torch.from_numpy(mo)).reshape(N, T, C6)
    m6 = torch.from_numpy(msk).repeat_interleave(D6, -1)
    lens = torch.from_numpy(ln).float()
    tvt = torch.from_numpy(tv).long()
    embt = torch.from_numpy(emb).to(dev)

    net = TextFlow(T, dim=a.dim, layers=a.layers).to(dev)
    print(f"params: {sum(p.numel() for p in net.parameters()) / 1e6:.1f} M", flush=True)
    os.makedirs(os.path.expanduser(a.out), exist_ok=True)
    out = os.path.expanduser(a.out)
    ckpt = os.path.join(out, "ckpt.pt")

    boost = None
    if a.boost:
        pat, fac = a.boost.rsplit("=", 1)
        boost = (re.compile(pat), float(fac))
    w = sample_weights(d, a.kimodo_share, boost)
    sampler = torch.utils.data.WeightedRandomSampler(torch.from_numpy(w), num_samples=a.samples_per_epoch,
                                                     replacement=True)
    srct = torch.from_numpy(d["src"].astype(np.int64))
    ds = torch.utils.data.TensorDataset(x1, m6, lens, tvt, srct)
    dl = torch.utils.data.DataLoader(ds, batch_size=a.batch, sampler=sampler, drop_last=True)
    opt = torch.optim.AdamW(net.parameters(), lr=a.lr, weight_decay=1e-4)
    sched = torch.optim.lr_scheduler.CosineAnnealingLR(opt, T_max=a.epochs * len(dl))
    start = 0
    if (a.resume or a.export_only) and os.path.exists(ckpt):
        ck = torch.load(ckpt, map_location=dev, weights_only=True)
        net.load_state_dict(ck["net"]); opt.load_state_dict(ck["opt"]); sched.load_state_dict(ck["sched"])
        start = ck["epoch"] + 1
        print(f"resumed at epoch {start}", flush=True)
    elif a.export_only and not a.init:
        sys.exit(f"--export-only needs trained weights: no {ckpt} and no --init")
    elif a.init:
        ck = torch.load(os.path.expanduser(a.init), map_location=dev, weights_only=True)
        net.load_state_dict(ck["net"])
        print(f"initialised from {a.init} (epoch {ck['epoch'] + 1})", flush=True)

    frames = torch.arange(T, device=dev).float()
    for ep in range(start, 0 if a.export_only else a.epochs):
        net.train()
        tot = torch.zeros((), device=dev); totv = torch.zeros((), device=dev); nb = 0
        for xb, mb, lb, tb, sb in dl:
            if a.crop_prob > 0:
                xb, lb = random_crop(xb, lb, sb, a.crop_prob, a.crop_min)
            xb, mb, lb = xb.to(dev), mb.to(dev), lb.to(dev)
            pick = torch.randint(0, tb.shape[1], (tb.shape[0],))
            txt = embt[tb[torch.arange(tb.shape[0]), pick].to(dev)]
            txt = txt * (torch.rand(txt.shape[0], device=dev) >= 0.1).float()[:, None]
            x0 = torch.randn_like(xb)
            t = torch.rand(xb.shape[0], device=dev)
            xt = (1 - t[:, None, None]) * x0 + t[:, None, None] * xb
            v = net(xt, t, txt, lb)
            fm = (frames[None] < lb[:, None]).float()[:, :, None]          # [B,T,1]
            mask = fm * mb[:, None, :]
            loss = ((v - (xb - x0)) ** 2 * mask).sum() / mask.sum()
            if a.vel_weight > 0:
                # Velocity of the predicted CLEAN motion vs the data: a pose-only loss lets the model
                # settle on a timid average (runs that barely move). Weighted by t, since the clean
                # estimate is meaningless at high noise.
                x1h = xt + (1 - t[:, None, None]) * v
                vm = mask[:, 1:] * mask[:, :-1]
                vl = ((((x1h[:, 1:] - x1h[:, :-1]) - (xb[:, 1:] - xb[:, :-1])) ** 2) * vm
                      * t[:, None, None]).sum() / vm.sum()
                loss = loss + a.vel_weight * vl
                totv += vl.detach()
            opt.zero_grad(set_to_none=True)
            loss.backward()
            torch.nn.utils.clip_grad_norm_(net.parameters(), 1.0)
            opt.step(); sched.step()
            tot += loss.detach(); nb += 1
        vtxt = f" vel {totv.item() / max(1, nb):.5f}" if a.vel_weight > 0 else ""
        print(f"ep {ep + 1}/{a.epochs} loss {tot.item() / max(1, nb):.4f}{vtxt}", flush=True)
        torch.save({"net": net.state_dict(), "opt": opt.state_dict(), "sched": sched.state_dict(), "epoch": ep},
                   ckpt)

    dead = (msk.max(0) <= 0).astype(np.float32)
    print(f"pinned to identity (never supervised): roles {np.nonzero(dead)[0].tolist()}", flush=True)
    export(net, T, a.steps, a.guidance, out, int(d["fps"]), d["canonRestDir"], embedt2m.INSTRUCT, dead)


if __name__ == "__main__":
    main()
