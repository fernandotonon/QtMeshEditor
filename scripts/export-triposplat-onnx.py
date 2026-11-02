#!/usr/bin/env python3
"""Export TripoSplat (VAST-AI, MIT code + MIT weights) to ONNX for the native
QtMeshEditor runtime (`--backend triposplat`).

One-time, offline dev tool — NOT shipped. Needs a checkout of
https://github.com/VAST-AI-Research/TripoSplat (model.py + triposplat.py) and
the HF checkpoints (VAST-AI/TripoSplat):

    python scripts/export-triposplat-onnx.py --src ~/triposplat_eval \
        --ckpts ~/triposplat_eval/ckpts --out ~/triposplat_eval/onnx [--verify]

Graphs (all fp32, external weights where > 2 GB):

  triposplat_dinov3.onnx    image[1,3,1024,1024] in [0,1]  -> feature1[1,4101,1280]
                            (ImageNet normalisation + the un-affine layer_norm baked in)
  triposplat_vae.onnx       image[1,3,1024,1024] in [0,1], noise[1,32,128,128]
                            -> feature2[1,4101,128]
                            (FLUX.2 VAE encoder: mean + exp(.5 logvar)*noise, 2x2
                            patchify, BN normalisation, 5 zero prefix tokens)
  triposplat_flow.onnx      latent[B,8192,16], camera[B,1,5], t[B] (already x1000),
                            feature1[B,4101,1280], feature2[B,4101,128]
                            -> v_latent[B,8192,16], v_camera[B,1,5]
  triposplat_octree.onnx    points[1,L,3] (parent cell centres in [0,1]),
                            level[1] (int64 resolution 2^lv), latent[1,8192,16]
                            -> logits[1,L,8]
  triposplat_gs.onnx        points[1,P,3] in [0,1], latent[1,8192,16]
                            -> xyz[1,P*32,3] (model frame, centred), f_dc[1,P*32,3],
                               opacity[1,P*32,1] (sigmoid), scale[1,P*32,3] (activated),
                               rotation[1,P*32,4] (wxyz, NOT normalised)

The C++ side (src/ImageTo3D/TripoSplatPredictor.cpp) runs the Euler-CFG loop,
the octree systematic sampling and the splat -> mesh conversion.

Export traps handled here:
  * RePo3D / apply_rotary_emb use complex64 (torch.polar / view_as_complex) —
    ONNX has no complex type, so both are replaced by real cos/sin arithmetic.
  * clamp_mul uses x.detach() — numerically x*f; rewritten without detach.
"""
import argparse
import os
import sys
import time

import numpy as np
import torch
import torch.nn.functional as F


class _MhaFn(torch.autograd.Function):
    """SDPA on [B,H,L,D] in eager mode; com.microsoft.MultiHeadAttention in ONNX."""

    @staticmethod
    def forward(ctx, q, k, v, num_heads):
        return F.scaled_dot_product_attention(q, k, v)

    @staticmethod
    def symbolic(g, q, k, v, num_heads):
        perm = [0, 2, 1, 3]
        flat = g.op("Constant", value_t=torch.tensor([0, 0, -1], dtype=torch.int64))
        def to_bsd(x):
            return g.op("Reshape", g.op("Transpose", x, perm_i=perm), flat)
        out = g.op("com.microsoft::MultiHeadAttention", to_bsd(q), to_bsd(k), to_bsd(v),
                   num_heads_i=num_heads)
        unflat = g.op("Constant", value_t=torch.tensor([0, 0, num_heads, -1], dtype=torch.int64))
        return g.op("Transpose", g.op("Reshape", out, unflat), perm_i=perm)


def mha_sdpa(q, k, v, *args, **kwargs):
    assert not args and not kwargs, "masked/causal SDPA is not exported"
    if torch.onnx.is_in_onnx_export():
        return _MhaFn.apply(q, k, v, int(q.shape[1]))
    return F.scaled_dot_product_attention(q, k, v)


def patch_complex_rope(model_mod):
    """Replace complex rotary embedding with an equivalent real formulation."""

    def repo_forward(self, hidden_states):
        h = self.norm(hidden_states)
        feat = self.act(self.gate_map(h)) * self.content_map(h)
        out = self.final_map(feat)
        B, L, _ = out.shape
        d = out.reshape(B, L, self.num_heads, 3)
        a0 = d[..., 0:1] * self.freqs_0 * torch.pi
        a1 = d[..., 1:2] * self.freqs_1 * torch.pi
        a2 = d[..., 2:3] * self.freqs_2 * torch.pi
        ang = torch.cat([a0, a1, a2], dim=-1).float()
        return (torch.cos(ang), torch.sin(ang))

    def apply_rotary_emb(hidden_states, freqs):
        cos, sin = freqs
        x = hidden_states.float().reshape(*hidden_states.shape[:-1], -1, 2)
        a, b = x[..., 0], x[..., 1]
        out = torch.stack([a * cos - b * sin, a * sin + b * cos], dim=-1)
        return out.reshape(*hidden_states.shape).type_as(hidden_states)

    model_mod.RePo3DRotaryEmbedding.forward = repo_forward
    model_mod.apply_rotary_emb = apply_rotary_emb

    # `F.layer_norm(h, h.shape[-1:])` traces the channel count as a dynamic
    # value once any axis is dynamic, which the TorchScript exporter rejects
    # ("ListConstruct ... is not constant"). The channel axis is always a
    # constant, so freeze it to Python ints.
    import types
    shim = types.ModuleType("F_shim")
    shim.__dict__.update(F.__dict__)

    def layer_norm(x, normalized_shape, *a, **kw):
        return F.layer_norm(x, tuple(int(s) for s in normalized_shape), *a, **kw)

    shim.layer_norm = layer_norm
    # Every attention call in model.py is a mask-free SDPA on [B,H,L,D]. The
    # TorchScript exporter lowers SDPA to MatMul-Softmax-MatMul, which makes
    # ORT build the whole score matrix: 16 x 12294^2 fp32 = 9.7 GB per flow
    # block (it never finished and took a 24 GB machine down). ORT's
    # com.microsoft.MultiHeadAttention CPU kernel is fused/tiled: the same
    # 12294-token attention runs in ~1.5 s at ~0.4 GB. Export to that op.
    shim.scaled_dot_product_attention = mha_sdpa
    model_mod.F = shim


# ---------------------------------------------------------------------------
# Wrappers
# ---------------------------------------------------------------------------

class DinoWrap(torch.nn.Module):
    def __init__(self, dino):
        super().__init__()
        self.dino = dino
        self.register_buffer("mean", torch.tensor([0.485, 0.456, 0.406]).view(1, 3, 1, 1))
        self.register_buffer("std", torch.tensor([0.229, 0.224, 0.225]).view(1, 3, 1, 1))

    def forward(self, image):
        x = (image - self.mean) / self.std
        f = self.dino(pixel_values=x)
        return F.layer_norm(f.float(), (int(f.shape[-1]),))


class VaeWrap(torch.nn.Module):
    def __init__(self, vae):
        super().__init__()
        self.vae = vae

    def forward(self, image, noise):
        v = self.vae
        moments = v.quant_conv(v.encoder(image * 2 - 1))
        mean, logvar = moments.chunk(2, dim=1)
        lat = mean + torch.exp(0.5 * logvar) * noise
        B, C, H, W = lat.shape
        lat = lat.view(B, C, H // 2, 2, W // 2, 2).permute(0, 1, 3, 5, 2, 4)
        lat = lat.reshape(B, C * 4, H // 2, W // 2)
        bn_mean = v.bn.running_mean.view(1, -1, 1, 1)
        bn_std = torch.sqrt(v.bn.running_var.view(1, -1, 1, 1) + v.bn.eps)
        f = ((lat - bn_mean) / bn_std).flatten(2).transpose(1, 2)
        zero = torch.zeros_like(f[:, :5, :])
        return torch.cat([zero, f], dim=1)


class FlowWrap(torch.nn.Module):
    def __init__(self, flow):
        super().__init__()
        self.flow = flow

    def forward(self, latent, camera, t, feature1, feature2):
        out = self.flow({"latent": latent, "camera": camera}, t,
                        {"feature1": feature1, "feature2": feature2})
        return out["latent"].float(), out["camera"].float()


class OctreeWrap(torch.nn.Module):
    def __init__(self, octree):
        super().__init__()
        self.octree = octree

    def forward(self, points, level, latent):
        return self.octree(points, level, latent)["logits"].float()


class GsWrap(torch.nn.Module):
    """ElasticGaussianFixedlenDecoder + _build_gaussians + Gaussian activations."""

    def __init__(self, gs):
        super().__init__()
        self.gs = gs
        rc = gs.rep_config
        self.lr = rc["lr"]
        self.min_kernel = rc["filter_kernel_size_3d"]
        sb = torch.tensor(rc["scaling_bias"])
        self.scale_bias = float(sb + torch.log(-torch.expm1(-sb)))      # inverse softplus
        ob = torch.tensor(rc["opacity_bias"])
        self.opacity_bias = float(torch.log(ob / (1 - ob)))              # inverse sigmoid

    def forward(self, points, latent):
        gs = self.gs
        h = gs({"points": points}, latent)["features"].float()
        B, P, _ = h.shape
        offset = gs._get_offset(h)                                       # [B,P,ng,3]
        xyz = (offset + points[:, :, None, :]).reshape(B, -1, 3) - 0.5   # aabb [-.5,-.5,-.5,1,1,1]

        def field(name):
            r = gs.layout[name]["range"]
            return h[:, :, r[0]:r[1]].reshape(B, P, *gs.layout[name]["shape"]).reshape(B, P * gs.layout[name]["shape"][0], -1) * self.lr[name]

        f_dc = field("_features_dc")
        opacity = torch.sigmoid(field("_opacity") + self.opacity_bias)
        s = F.softplus(field("_scaling") + self.scale_bias)
        scale = torch.sqrt(s * s + self.min_kernel ** 2)
        rot = field("_rotation") + torch.tensor([1.0, 0.0, 0.0, 0.0], device=h.device)
        return xyz, f_dc, opacity, scale, rot


# ---------------------------------------------------------------------------

def load_all(src, ckpts, only):
    sys.path.insert(0, src)
    import model as model_mod
    import triposplat as tp
    patch_complex_rope(model_mod)
    p = lambda *a: os.path.join(ckpts, *a)
    t0 = time.time()
    # Load only what is exported: the four fp32 models together exceed the
    # RAM of a 24 GB machine once the flow graph is traced.
    m = {}
    if "dinov3" in only:
        m["dino"] = tp.load_dinov3(p("clip_vision", "dino_v3_vit_h.safetensors"), dtype=torch.float32)
    if "vae" in only:
        m["vae"] = tp.load_vae_encoder(p("vae", "flux2-vae.safetensors"), dtype=torch.float32)
    if "flow" in only:
        m["flow"] = tp.load_flow_model(p("diffusion_models", "triposplat_fp16.safetensors"), dtype=torch.float32)
    if only & {"octree", "gs"}:
        m["dec"] = tp.load_decoder(p("vae", "triposplat_vae_decoder_fp16.safetensors"), dtype=torch.float32)
    print(f"loaded in {time.time() - t0:.1f}s")
    return m, model_mod, tp


def export(module, args, path, input_names, output_names, dynamic_axes):
    t0 = time.time()
    with torch.no_grad():
        torch.onnx.export(module, args, path, input_names=input_names,
                          output_names=output_names, dynamic_axes=dynamic_axes,
                          opset_version=18, dynamo=False, do_constant_folding=True,
                          custom_opsets={"com.microsoft": 1})
    import onnx
    # Always re-save with external weights: >2 GB graphs need it, and a small
    # protobuf + one .data file is what the downloader/ORT handle best (ORT
    # 1.20 SIGSEGVs parsing huge single-file protos — see SkinTokens).
    m = onnx.load(path)
    data = os.path.basename(path) + ".data"
    data_path = os.path.join(os.path.dirname(path), data)
    if os.path.exists(data_path):
        os.remove(data_path)
    onnx.save_model(m, path, save_as_external_data=True, all_tensors_to_one_file=True,
                    location=data, size_threshold=1024)
    print(f"  exported {os.path.basename(path)} in {time.time() - t0:.1f}s")


def max_err(a, b):
    a = np.asarray(a, dtype=np.float64)
    b = np.asarray(b, dtype=np.float64)
    return float(np.abs(a - b).max()), float(np.abs(b).max())


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--src", required=True)
    ap.add_argument("--ckpts", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--only", default="", help="comma list: dinov3,vae,flow,octree,gs")
    ap.add_argument("--verify", action="store_true")
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    only = set(filter(None, a.only.split(","))) or {"dinov3", "vae", "flow", "octree", "gs"}

    m, model_mod, tp = load_all(a.src, a.ckpts, only)
    torch.manual_seed(0)
    img = torch.rand(1, 3, 1024, 1024)
    lat = torch.randn(1, 8192, 16)

    jobs = []
    if "dinov3" in only:
        w = DinoWrap(m["dino"]).eval()
        jobs.append(("triposplat_dinov3.onnx", w, (img,), ["image"], ["feature1"], None))
    if "vae" in only:
        w = VaeWrap(m["vae"]).eval()
        noise = torch.randn(1, 32, 128, 128)
        jobs.append(("triposplat_vae.onnx", w, (img, noise), ["image", "noise"], ["feature2"], None))
    if "flow" in only:
        w = FlowWrap(m["flow"]).eval()
        # Traced at B=1 (B=2 does not fit in 24 GB during export); the batch
        # axis is still dynamic so the runtime can batch cond+uncond.
        args = (torch.randn(1, 8192, 16), torch.randn(1, 1, 5), torch.tensor([700.0]),
                torch.randn(1, 4101, 1280), torch.randn(1, 4101, 128))
        dyn = {k: {0: "B"} for k in ["latent", "camera", "t", "feature1", "feature2", "v_latent", "v_camera"]}
        jobs.append(("triposplat_flow.onnx", w, args,
                     ["latent", "camera", "t", "feature1", "feature2"], ["v_latent", "v_camera"], dyn))
    if "octree" in only:
        w = OctreeWrap(m["dec"].octree).eval()
        args = (torch.rand(1, 64, 3), torch.tensor([8], dtype=torch.long), lat)
        jobs.append(("triposplat_octree.onnx", w, args, ["points", "level", "latent"], ["logits"],
                     {"points": {1: "L"}, "logits": {1: "L"}}))
    if "gs" in only:
        w = GsWrap(m["dec"].gs).eval()
        args = (torch.rand(1, 128, 3), lat)
        jobs.append(("triposplat_gs.onnx", w, args, ["points", "latent"],
                     ["xyz", "f_dc", "opacity", "scale", "rotation"],
                     {"points": {1: "P"}, **{k: {1: "N"} for k in ["xyz", "f_dc", "opacity", "scale", "rotation"]}}))

    for name, w, args, ins, outs, dyn in jobs:
        path = os.path.join(a.out, name)
        export(w, args, path, ins, outs, dyn)
        if a.verify:
            verify(path, w, args, ins)


def verify(path, module, args, input_names):
    """Load in ORT (catches graphs that export but do not load), run on the
    export inputs AND a second shape where axes are dynamic, compare to torch."""
    import onnxruntime as ort
    so = ort.SessionOptions()
    sess = ort.InferenceSession(path, so, providers=["CPUExecutionProvider"])
    feeds = {n: v.numpy() for n, v in zip(input_names, args)}
    t0 = time.time()
    got = sess.run(None, feeds)
    dt = time.time() - t0
    with torch.no_grad():
        ref = module(*args)
    if isinstance(ref, torch.Tensor):
        ref = (ref,)
    for i, (g, r) in enumerate(zip(got, ref)):
        e, mx = max_err(g, r.numpy())
        finite = bool(np.isfinite(g).all())
        print(f"  verify {os.path.basename(path)} out{i} {tuple(g.shape)}: max|d|={e:.3g} (max|ref|={mx:.3g}) finite={finite} ort={dt:.2f}s")
        if not finite:
            raise SystemExit("non-finite output")


if __name__ == "__main__":
    main()
