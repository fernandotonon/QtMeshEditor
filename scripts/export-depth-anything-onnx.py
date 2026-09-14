#!/usr/bin/env python3
"""Export Depth-Anything-V2-Small to ONNX for QtMeshEditor (#1018, epic #818 C4).

ONE-TIME, OFFLINE DEV TOOL — not shipped with the app, not run at runtime.
Mirrors scripts/export-pbrify-onnx.py / export-skintokens-onnx.py.

LICENCE GATE (the reason this script refuses anything but Small):
    Depth-Anything-V2-Small  apache-2.0    -> redistributable, SHIPPED
    Depth-Anything-V2-Base   cc-by-nc-4.0  -> non-commercial, REJECTED
    Depth-Anything-V2-Large  cc-by-nc-4.0  -> non-commercial, REJECTED
Verified against the HF model API at export time. The project's bar is
permissive redistribution (Homebrew/Snap/WinGet/Docker), so the NC variants can
never ship no matter how much better they look. The script hard-fails on them
rather than leaving that to reviewer vigilance.

Contract the C++ side (src/PhotoDepth.{h,cpp}) relies on:
    input   pixel_values  float32 [1,3,H,W]   ImageNet-normalised RGB
    output  predicted_depth float32 [1,H,W]   RELATIVE inverse depth,
                                              larger = NEARER
H and W are dynamic axes (the model is a ViT with interpolated position
embeddings), but must be multiples of 14 — the patch size. The C++ side resizes
to a multiple of 14 and normalises the output to 0..255 itself, matching
MeshDepthRenderer's near=bright convention.
"""
import argparse
import json
import sys
import urllib.request

REPO_SMALL = "depth-anything/Depth-Anything-V2-Small-hf"
ALLOWED_LICENSES = {"apache-2.0"}
PATCH = 14


def check_license(repo: str) -> str:
    """Refuse to export a model whose licence we cannot redistribute."""
    url = f"https://huggingface.co/api/models/{repo}"
    with urllib.request.urlopen(url, timeout=60) as r:
        meta = json.load(r)
    lic = (meta.get("cardData") or {}).get("license") or "unknown"
    if lic not in ALLOWED_LICENSES:
        sys.exit(
            f"REFUSING to export {repo}: license '{lic}' is not in the "
            f"permissive allow-list {sorted(ALLOWED_LICENSES)}.\n"
            "Depth-Anything-V2 Base/Large are cc-by-nc-4.0 and must never be "
            "shipped (see THIRD_PARTY_AI_MODELS.md). Only the Small variant "
            "clears the bar."
        )
    return lic


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--repo", default=REPO_SMALL,
                    help="HF model id (must be permissively licensed)")
    ap.add_argument("-o", "--out", default="da2_small.onnx")
    ap.add_argument("--opset", type=int, default=18)
    ap.add_argument("--size", type=int, default=518,
                    help="tracing resolution; must be a multiple of 14")
    ap.add_argument("--tolerance", type=float, default=2e-3,
                    help="max abs torch-vs-ORT difference accepted")
    args = ap.parse_args()

    if args.size % PATCH:
        sys.exit(f"--size must be a multiple of {PATCH} (ViT patch size)")

    lic = check_license(args.repo)
    print(f"[licence] {args.repo}: {lic} — OK")

    import numpy as np
    import torch
    from transformers import AutoModelForDepthEstimation

    print(f"[load] {args.repo}")
    model = AutoModelForDepthEstimation.from_pretrained(args.repo)
    model.eval()

    class Wrap(torch.nn.Module):
        """Return the bare depth tensor, resized to follow the INPUT size.

        The HF head ends with

            nn.functional.interpolate(
                ..., (int(patch_height * self.patch_size), int(...)), ...)

        and those `int()` casts turn the target size into a TorchScript
        constant. The exported graph then declares dynamic height/width but
        always emits the tracing resolution — measured: a 462x462 input still
        produced a 518x518 depth map. Callers would silently get a depth map of
        the wrong resolution for every size but the traced one.

        Re-interpolating here to `pixel_values.shape[-2:]` keeps the size as a
        traced tensor shape rather than a baked constant, so the output really
        does follow the input. At the tracing size this is a no-op resize.
        """
        def __init__(self, m):
            super().__init__()
            self.m = m

        def forward(self, pixel_values):
            depth = self.m(pixel_values=pixel_values).predicted_depth
            if depth.dim() == 3:
                depth = depth.unsqueeze(1)          # [B,1,H,W] for interpolate
            depth = torch.nn.functional.interpolate(
                depth, size=pixel_values.shape[-2:],
                mode="bilinear", align_corners=False)
            return depth.squeeze(1)                 # back to [B,H,W]

    wrapped = Wrap(model).eval()
    example = torch.randn(1, 3, args.size, args.size, dtype=torch.float32)

    print(f"[export] opset {args.opset} -> {args.out}")
    torch.onnx.export(
        wrapped, (example,), args.out,
        input_names=["pixel_values"], output_names=["predicted_depth"],
        # Batch stays dynamic; H/W are PINNED to the tracing size on purpose.
        # A dynamic-axis export traces clean but drifts badly away from the
        # traced resolution: the ViT position-embedding interpolation is only
        # partly captured, measured at 2.3e-02 relative error at 462x462 and
        # 4.9e-02 at 392x392 (vs 2.7e-06 at 518x518) on smooth images. Since
        # the reference preprocessor resizes every input to 518x518 anyway
        # (preprocessor_config.json: size 518, keep_aspect_ratio,
        # ensure_multiple_of 14), a fixed shape costs nothing and removes a
        # silent-wrong-answer failure mode. The C++ side letterboxes to 518.
        dynamic_axes={"pixel_values": {0: "batch"},
                      "predicted_depth": {0: "batch"}},
        opset_version=args.opset, do_constant_folding=True, dynamo=False,
    )

    # ---- PARITY: the step the UniRig export (#1025) skipped ----------------
    # That export shipped a graph emitting 100% NaN latents and nobody noticed
    # until the feature was investigated months later. Never upload a graph
    # whose output has not been compared against torch on real input.
    import onnxruntime as ort
    print("[parity] torch vs onnxruntime")
    with torch.no_grad():
        ref = wrapped(example).cpu().numpy()
    sess = ort.InferenceSession(args.out, providers=["CPUExecutionProvider"])
    got = sess.run(None, {"pixel_values": example.numpy()})[0]

    if got.shape != ref.shape:
        sys.exit(f"shape mismatch: torch {ref.shape} vs ORT {got.shape}")
    if not np.isfinite(got).all():
        bad = int((~np.isfinite(got)).sum())
        sys.exit(f"ONNX output has {bad}/{got.size} non-finite values — "
                 "the export is numerically broken (this is exactly the "
                 "UniRig #1025 failure; do NOT upload)")
    err = float(np.abs(ref - got).max())
    rng = float(ref.max() - ref.min())
    print(f"  shape {got.shape}  max|diff| {err:.3e}  (output range {rng:.3f})")
    if err > args.tolerance:
        sys.exit(f"parity FAILED: {err:.3e} > tolerance {args.tolerance:.3e}")

    # Contract check: the input shape must be PINNED. A graph that advertises
    # dynamic H/W here would be accepted by the C++ side and then return
    # quietly wrong depth at any other size (see the dynamic_axes note above).
    decl = sess.get_inputs()[0].shape
    if decl[2] != args.size or decl[3] != args.size:
        sys.exit(f"expected pinned {args.size}x{args.size} input, got {decl} — "
                 "a dynamic-axis export drifts away from the traced size")
    print(f"  input shape pinned: {decl}")

    # Parity on a SMOOTH image too. White noise is the easiest case for a
    # resize mismatch to hide in; a structured image is what the model will
    # actually see.
    yy, xx = np.mgrid[0:args.size, 0:args.size].astype(np.float32) / args.size
    smooth = np.stack([np.sin(6 * xx) * np.cos(5 * yy),
                       np.cos(4 * xx + yy), xx * yy], 0)[None].astype(np.float32)
    with torch.no_grad():
        ref3 = wrapped(torch.from_numpy(smooth)).cpu().numpy()
    got3 = sess.run(None, {"pixel_values": smooth})[0]
    if not np.isfinite(got3).all():
        sys.exit("smooth-image output is non-finite — do NOT upload")
    err3 = float(np.abs(ref3 - got3).max())
    print(f"[parity] smooth image  max|diff| {err3:.3e}")
    if err3 > args.tolerance:
        sys.exit(f"smooth-image parity FAILED: {err3:.3e}")

    print(f"[ok] {args.out} — parity verified, safe to host")


if __name__ == "__main__":
    main()
