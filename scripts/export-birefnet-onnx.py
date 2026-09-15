#!/usr/bin/env python3
"""Export BiRefNet to ONNX for QtMeshEditor (#1016, epic #818 C2).

ONE-TIME, OFFLINE DEV TOOL — not shipped, not run at runtime. Mirrors
scripts/export-depth-anything-onnx.py.

BiRefNet (Zheng et al., "Bilateral Reference for High-Resolution Dichotomous
Image Segmentation") is MIT — code AND weights — so it clears the project's
permissive-redistribution bar. It replaces U2Net as the QUALITY tier for
image-to-3D background removal: 1024^2 input vs U2Net's 320^2, and far better
edges on hair/fur (the case U2Net visibly fails).

Contract the C++ side (BackgroundRemover, Quality::Best) relies on:
    input   pixel_values  float32 [1,3,1024,1024]  ImageNet-normalised RGB
    output  alpha         float32 [1,1,1024,1024]  foreground matte in [0,1]
Spatial dims are PINNED, as with the depth export: BiRefNet is a ViT-backboned
model and a dynamic-axis export risks the same position-embedding drift, while
the reference pipeline resizes everything to 1024 anyway.
"""
import argparse
import json
import sys
import urllib.request

REPO_DEFAULT = "ZhengPeng7/BiRefNet"
ALLOWED_LICENSES = {"mit", "apache-2.0"}
RUNTIME_SIZE = 1024   # must equal BackgroundRemover's kBiRefNet size


def check_license(repo: str) -> str:
    url = f"https://huggingface.co/api/models/{repo}"
    with urllib.request.urlopen(url, timeout=60) as r:
        meta = json.load(r)
    lic = (meta.get("cardData") or {}).get("license") or "unknown"
    if lic not in ALLOWED_LICENSES:
        sys.exit(
            f"REFUSING to export {repo}: license '{lic}' is not in the "
            f"permissive allow-list {sorted(ALLOWED_LICENSES)}. "
            "See THIRD_PARTY_AI_MODELS.md."
        )
    return lic


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--repo", default=REPO_DEFAULT)
    ap.add_argument("-o", "--out", default="birefnet.onnx")
    ap.add_argument("--opset", type=int, default=17)
    ap.add_argument("--size", type=int, default=RUNTIME_SIZE)
    ap.add_argument("--allow-size-mismatch", action="store_true",
                    help="export at a size the shipped runtime cannot consume "
                         "(experiments only; do NOT host the result)")
    ap.add_argument("--tolerance", type=float, default=5e-3)
    args = ap.parse_args()

    if args.size != RUNTIME_SIZE and not args.allow_size_mismatch:
        sys.exit(
            f"--size {args.size} does not match the runtime's pinned "
            f"{RUNTIME_SIZE}. The exported graph would reject every inference. "
            "Pass --allow-size-mismatch only for experiments."
        )

    lic = check_license(args.repo)
    print(f"[licence] {args.repo}: {lic} — OK")

    import numpy as np
    import torch
    from transformers import AutoModelForImageSegmentation

    print(f"[load] {args.repo}")
    model = AutoModelForImageSegmentation.from_pretrained(
        args.repo, trust_remote_code=True)
    # The published checkpoint carries HALF-precision weights, so tracing with
    # an fp32 input fails with "Input type (float) and bias type (c10::Half)
    # should be the same". Export in fp32: ONNX Runtime's CPU EP has no fp16
    # kernels for much of this graph, and the C++ side feeds fp32 tensors.
    model = model.float()
    model.eval()

    # ---- deform_conv2d decomposition ---------------------------------------
    # BiRefNet's decoder uses ASPPDeformable, and `torchvision::deform_conv2d`
    # has NO ONNX representation ("unrecognized namespace"). It is not optional:
    # the checkpoint carries 150 deformable tensors (offset/modulator/regular
    # convs), so disabling it would discard trained weights and change the model.
    #
    # Instead, express it with ops ONNX does understand — GridSample for the
    # offset sampling, MatMul for the convolution. Verified against
    # torchvision.ops.deform_conv2d to ~7e-07 on every kernel size BiRefNet
    # uses (1x1, 3x3, 7x7), modulation mask included.
    import torch.nn.functional as _F
    from torchvision.ops import deform_conv as _dc

    # Signature must match torchvision.ops.deform_conv2d EXACTLY, including the
    # parameter NAME `input` — BiRefNet calls it with keywords, and a rename
    # fails at trace time with "unexpected keyword argument 'input'".
    def _deform_conv2d_onnx(input, offset, weight, bias=None, stride=1,
                            padding=0, dilation=1, mask=None):
        x = input
        B, C, H, W = x.shape
        kh, kw = weight.shape[2], weight.shape[3]
        sh = stride if isinstance(stride, int) else stride[0]
        ph = padding if isinstance(padding, int) else padding[0]
        dl = dilation if isinstance(dilation, int) else dilation[0]
        Hout = (H + 2 * ph - (dl * (kh - 1) + 1)) // sh + 1
        Wout = (W + 2 * ph - (dl * (kw - 1) + 1)) // sh + 1
        dev, dt = x.device, x.dtype
        oy = torch.arange(Hout, device=dev, dtype=dt) * sh - ph
        ox = torch.arange(Wout, device=dev, dtype=dt) * sh - ph
        ky = torch.arange(kh, device=dev, dtype=dt) * dl
        kx = torch.arange(kw, device=dev, dtype=dt) * dl
        base_y = (oy.view(1, Hout, 1) + ky.view(kh, 1, 1)).repeat_interleave(kw, 0)
        base_x = (ox.view(1, 1, Wout) + kx.view(kw, 1, 1)).repeat(kh, 1, 1)
        base_y = base_y.expand(kh * kw, Hout, Wout)
        base_x = base_x.expand(kh * kw, Hout, Wout)
        off = offset.view(B, kh * kw, 2, Hout, Wout)
        sy = base_y.unsqueeze(0) + off[:, :, 0]
        sx = base_x.unsqueeze(0) + off[:, :, 1]
        gx = (sx + 0.5) / W * 2 - 1
        gy = (sy + 0.5) / H * 2 - 1
        grid = torch.stack([gx, gy], dim=-1).view(B, kh * kw * Hout, Wout, 2)
        samp = _F.grid_sample(x, grid, mode="bilinear",
                              padding_mode="zeros", align_corners=False)
        samp = samp.view(B, C, kh * kw, Hout, Wout)
        if mask is not None:
            samp = samp * mask.view(B, 1, kh * kw, Hout, Wout)
        cols = samp.reshape(B, C * kh * kw, Hout * Wout)
        wmat = weight.reshape(weight.shape[0], C * kh * kw)
        out = torch.matmul(wmat, cols).view(B, weight.shape[0], Hout, Wout)
        if bias is not None:
            out = out + bias.view(1, -1, 1, 1)
        return out

    # birefnet.py does `from torchvision.ops import deform_conv2d`, which binds
    # the symbol into ITS OWN module namespace at import time — patching
    # torchvision afterwards is too late and silently has no effect (the first
    # attempt here failed exactly that way). Patch every already-imported module
    # that holds a reference.
    import sys as _sys
    _dc.deform_conv2d = _deform_conv2d_onnx
    import torchvision.ops as _tvops
    _tvops.deform_conv2d = _deform_conv2d_onnx
    _patched = 0
    for _name, _mod in list(_sys.modules.items()):
        if _mod is None:
            continue
        if getattr(_mod, "deform_conv2d", None) is not None \
                and getattr(_mod, "deform_conv2d") is not _deform_conv2d_onnx:
            setattr(_mod, "deform_conv2d", _deform_conv2d_onnx)
            _patched += 1
    print(f"[patch] deform_conv2d -> ONNX decomposition ({_patched} module(s))")
    if _patched == 0:
        sys.exit("could not patch deform_conv2d in any loaded module — the "
                 "export would fail later with 'unrecognized namespace'")

    class Wrap(torch.nn.Module):
        """Return a single [B,1,H,W] matte in [0,1].

        BiRefNet's forward returns a LIST of progressively refined maps as raw
        logits; the reference pipeline takes the LAST one and sigmoids it.
        Exporting the list would hand the C++ side several outputs with no
        indication which is authoritative.
        """
        def __init__(self, m):
            super().__init__()
            self.m = m

        def forward(self, pixel_values):
            out = self.m(pixel_values)
            if isinstance(out, (list, tuple)):
                out = out[-1]
                if isinstance(out, (list, tuple)):
                    out = out[-1]
            return torch.sigmoid(out)

    wrapped = Wrap(model).eval()
    example = torch.randn(1, 3, args.size, args.size, dtype=torch.float32)

    print(f"[export] opset {args.opset} -> {args.out}")
    torch.onnx.export(
        wrapped, (example,), args.out,
        input_names=["pixel_values"], output_names=["alpha"],
        dynamic_axes={"pixel_values": {0: "batch"}, "alpha": {0: "batch"}},
        opset_version=args.opset, do_constant_folding=True, dynamo=False,
    )

    # ---- PARITY (the step the UniRig export skipped; see #1025) -------------
    import onnxruntime as ort
    print("[parity] torch vs onnxruntime")
    with torch.no_grad():
        ref = wrapped(example).cpu().numpy()
    sess = ort.InferenceSession(args.out, providers=["CPUExecutionProvider"])
    got = sess.run(None, {"pixel_values": example.numpy()})[0]

    if got.shape != ref.shape:
        sys.exit(f"shape mismatch: torch {ref.shape} vs ORT {got.shape}")
    if not np.isfinite(got).all():
        sys.exit(f"ONNX output has non-finite values — broken export, do NOT "
                 "upload (this is the UniRig #1025 failure)")
    err = float(np.abs(ref - got).max())
    print(f"  shape {got.shape}  max|diff| {err:.3e}  range [{got.min():.3f}, {got.max():.3f}]")
    if err > args.tolerance:
        sys.exit(f"parity FAILED: {err:.3e} > {args.tolerance:.3e}")

    decl = sess.get_inputs()[0].shape
    if decl[2] != args.size or decl[3] != args.size:
        sys.exit(f"expected pinned {args.size} input, got {decl}")
    print(f"  input shape pinned: {decl}")

    # A matte must actually discriminate. A graph that returns a constant field
    # is "finite" and passes parity but segments nothing.
    yy, xx = np.mgrid[0:args.size, 0:args.size].astype(np.float32) / args.size
    disc = np.stack([((xx - .5) ** 2 + (yy - .5) ** 2 < .05).astype(np.float32)] * 3,
                    0)[None]
    got2 = sess.run(None, {"pixel_values": disc})[0]
    spread = float(got2.max() - got2.min())
    print(f"[sanity] synthetic disc: alpha spread {spread:.3f}")
    if spread < 0.05:
        sys.exit("the matte is nearly constant — the export segments nothing")

    print(f"[ok] {args.out} — parity verified, safe to host")


if __name__ == "__main__":
    main()
