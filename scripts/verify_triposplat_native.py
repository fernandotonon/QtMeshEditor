#!/usr/bin/env python3
"""Reference replay of the native TripoSplat runtime (numpy + onnxruntime only).

Implements exactly the algorithm src/ImageTo3D/TripoSplatPredictor.cpp runs, so
it doubles as (a) an end-to-end check of the exported graphs and (b) the
fixture generator for C++ parity:

    python scripts/verify_triposplat_native.py --onnx <dir> --image prepared.png \
        --steps 10 --guidance 1 --out run.npz [--fixture fixture.npz]

`--fixture` writes every random draw (VAE noise, flow noise, octree/jitter
uniforms) so the C++ side can replay the same run (QTMESH_TRIPOSPLAT_FIXTURE).
The image must already be the 1024x1024 prepared composite (subject on black).
"""
import argparse
import os
import time

import numpy as np
import onnxruntime as ort
from PIL import Image

Q_TOKENS, IN_CH, CAM_CH = 8192, 16, 5
GAUSSIANS_PER_POINT = 32
OCTREE_LEVELS = 8
CHILD_OFFSETS = np.array([[i, j, k] for k in (0, 1) for j in (0, 1) for i in (0, 1)], np.int64)


def session(path, threads):
    so = ort.SessionOptions()
    so.intra_op_num_threads = threads
    return ort.InferenceSession(path, so, providers=["CPUExecutionProvider"])


def t_schedule(steps, shift):
    lin = np.linspace(1, 0, steps + 1)
    return shift * lin / (1 + (shift - 1) * lin)


def systematic_counts(probs, counts, uniforms):
    """sample_probs(algo='systematic') with the per-row uniform supplied.

    probs [R,8], counts [R], uniforms [R] in [0,1). Returns [R,8] int counts."""
    probs = np.clip(probs.astype(np.float64), 0, None)
    s = probs.sum(1, keepdims=True)
    probs = np.where(s == 0, 1.0 / probs.shape[1], probs / np.maximum(s, 1))
    cdf = np.minimum(np.cumsum(probs, 1), 1.0 - 1e-12)
    out = np.zeros(probs.shape, np.int64)
    for r in range(probs.shape[0]):
        n = int(counts[r])
        if n == 0:
            continue
        us = np.minimum(uniforms[r] / n + np.arange(n) / n, 1.0 - 1e-12)
        idx = np.minimum(np.searchsorted(cdf[r], us, side="left"), probs.shape[1] - 1)
        np.add.at(out[r], idx, 1)
    return out


def octree_points(octree, latent, num_points, rng_u, rng_jitter):
    coords = np.zeros((1, 3), np.int64)
    counts = np.array([num_points], np.int64)
    for lv in range(1, OCTREE_LEVELS + 1):
        res_p, res = 1 << (lv - 1), 1 << lv
        parent = ((coords.astype(np.float32) + 0.5) / res_p)[None]
        logits = octree.run(None, {"points": parent, "level": np.array([res], np.int64),
                                   "latent": latent})[0][0].astype(np.float64)
        logits -= logits.max(1, keepdims=True)
        probs = np.exp(logits)
        probs /= probs.sum(1, keepdims=True)
        sampled = systematic_counts(probs, counts, rng_u(len(counts)))
        child = (coords[:, None, :] * 2 + CHILD_OFFSETS[None]).reshape(-1, 3)
        sampled = sampled.reshape(-1)
        keep = sampled > 0
        coords, counts = child[keep], sampled[keep]
    pts = np.repeat(coords, counts, axis=0).astype(np.float32)
    pts = (pts + rng_jitter(pts.shape)) / (1 << OCTREE_LEVELS)
    return pts[None].astype(np.float32)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--onnx", required=True)
    ap.add_argument("--image", required=True)
    ap.add_argument("--steps", type=int, default=10)
    ap.add_argument("--guidance", type=float, default=1.0)
    ap.add_argument("--shift", type=float, default=3.0)
    ap.add_argument("--gaussians", type=int, default=262144)
    ap.add_argument("--seed", type=int, default=42)
    ap.add_argument("--threads", type=int, default=max(1, os.cpu_count() - 1))
    ap.add_argument("--out", required=True)
    ap.add_argument("--fixture", default="")
    a = ap.parse_args()
    rng = np.random.default_rng(a.seed)
    draws = {}

    def g(name):
        return os.path.join(a.onnx, f"triposplat_{name}.onnx")

    img = np.asarray(Image.open(a.image).convert("RGB").resize((1024, 1024)), np.float32) / 255.0
    img = img.transpose(2, 0, 1)[None].copy()
    T = {}

    t0 = time.time()
    f1 = session(g("dinov3"), a.threads).run(None, {"image": img})[0]
    T["dinov3"] = time.time() - t0
    t0 = time.time()
    draws["vae_noise"] = rng.standard_normal((1, 32, 128, 128)).astype(np.float32)
    f2 = session(g("vae"), a.threads).run(None, {"image": img, "noise": draws["vae_noise"]})[0]
    T["vae"] = time.time() - t0

    flow = session(g("flow"), a.threads)
    draws["latent_noise"] = rng.standard_normal((1, Q_TOKENS, IN_CH)).astype(np.float32)
    draws["camera_noise"] = rng.standard_normal((1, 1, CAM_CH)).astype(np.float32)
    lat, cam = draws["latent_noise"].copy(), draws["camera_noise"].copy()
    ts = t_schedule(a.steps, a.shift)
    t0 = time.time()
    for i in range(a.steps):
        t, tp = ts[i], ts[i + 1]
        feeds = dict(latent=lat, camera=cam, t=np.array([1000 * t], np.float32), feature1=f1, feature2=f2)
        vl, vc = flow.run(None, feeds)
        if a.guidance > 1:
            nf = dict(feeds, feature1=np.zeros_like(f1), feature2=np.zeros_like(f2))
            nvl, nvc = flow.run(None, nf)
            vl = a.guidance * vl - (a.guidance - 1) * nvl
            vc = a.guidance * vc - (a.guidance - 1) * nvc
        dt = np.float32(t - tp)
        lat = (lat - vl * dt).astype(np.float32)
        cam = (cam - vc * dt).astype(np.float32)
        print(f"step {i + 1}/{a.steps} {time.time() - t0:.1f}s", flush=True)
    T["flow"] = time.time() - t0
    del flow

    t0 = time.time()
    octree = session(g("octree"), a.threads)
    us, js = [], []

    def rng_u(n):
        # float32-rounded: the C++ fixture reader takes float32 arrays only,
        # so both sides must use exactly these values.
        u = rng.random(n).astype(np.float32).astype(np.float64)
        us.append(u)
        return u

    def rng_j(shape):
        j = rng.random(shape).astype(np.float32)
        js.append(j)
        return j

    pts = octree_points(octree, lat, a.gaussians // GAUSSIANS_PER_POINT, rng_u, rng_j)
    T["octree"] = time.time() - t0
    t0 = time.time()
    xyz, f_dc, opacity, scale, rot = session(g("gs"), a.threads).run(None, {"points": pts, "latent": lat})
    T["gs"] = time.time() - t0
    print("timings", {k: round(v, 1) for k, v in T.items()}, "total", round(sum(T.values()), 1))
    np.savez(a.out, xyz=xyz[0], f_dc=f_dc[0], opacity=opacity[0], scale=scale[0], rotation=rot[0],
             latent=lat, points=pts[0])
    if a.fixture:
        draws["octree_uniforms"] = np.concatenate(us).astype(np.float32)
        draws["octree_jitter"] = np.concatenate([j.reshape(-1) for j in js]).astype(np.float32)
        np.savez(a.fixture, **draws, feature1=f1, feature2=f2, latent=lat, camera=cam, points=pts[0],
                 xyz=xyz[0], opacity=opacity[0])


if __name__ == "__main__":
    main()
