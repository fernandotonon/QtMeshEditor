#!/usr/bin/env python3
"""Fetch + VERIFY the LaMa inpainting ONNX graph for QtMeshEditor (#1017, epic #818 C3).

ONE-TIME, OFFLINE DEV TOOL — not shipped, not run at runtime. Mirrors
scripts/export-birefnet-onnx.py and scripts/export-depth-anything-onnx.py.

Unlike those two, this does NOT run a torch export: Carve/LaMa-ONNX already
publishes converted graphs, so the job here is to pick the right one and PROVE
it works before hosting. That distinction matters — #1025 shipped a hosted graph
that loaded fine and emitted 100% NaN for months.

LaMa (Suvorov et al., "Resolution-robust Large Mask Inpainting with Fourier
Convolutions", WACV 2022) is Apache-2.0 for code AND weights, so it clears the
project's permissive-redistribution bar. See THIRD_PARTY_AI_MODELS.md.

WHICH FILE: the repo publishes two graphs, and the one named plainly `lama.onnx`
is BROKEN — it fails ONNX Runtime shape inference on a DFT node (LaMa's Fourier
convolution) and cannot be loaded at all:

    Node (..._FourierUnit_..._DFT_20) Op (DFT) [ShapeInferenceError]

`lama_fp32.onnx` loads and runs correctly. We host it under the plain name
`lama.onnx`, which is what the C++ side downloads.

Measured contract the C++ side (TextureInpaint) relies on:
    input   image   float32 [1,3,512,512]  RGB scaled to [0,1]
    input   mask    float32 [1,1,512,512]  1 = inpaint, 0 = keep
    output  output  float32 [1,3,512,512]  RGB ALREADY IN 0..255

Note the asymmetry — in [0,1], out 0..255. It is not documented upstream; it was
measured. Feeding an un-scaled image produces a near-white image (mean 255.0 vs
a correct 156.0) with no error raised, so this script asserts it.
"""
import argparse
import hashlib
import json
import sys
import urllib.request

REPO_DEFAULT = "Carve/LaMa-ONNX"
# The plainly-named lama.onnx in this repo is the broken one; see the docstring.
SOURCE_FILE = "lama_fp32.onnx"
ALLOWED_LICENSES = {"apache-2.0", "mit"}
RUNTIME_SIZE = 512   # must equal TextureInpaint::kInputSize


def check_license(repo: str) -> str:
    url = f"https://huggingface.co/api/models/{repo}"
    with urllib.request.urlopen(url, timeout=60) as r:
        meta = json.load(r)
    lic = (meta.get("cardData") or {}).get("license") or "unknown"
    if lic not in ALLOWED_LICENSES:
        sys.exit(
            f"REFUSING to fetch {repo}: license '{lic}' is not in the "
            f"permissive allow-list {sorted(ALLOWED_LICENSES)}. "
            "See THIRD_PARTY_AI_MODELS.md."
        )
    return lic


def fetch(repo: str, name: str, dst: str) -> None:
    url = f"https://huggingface.co/{repo}/resolve/main/{name}"
    print(f"[fetch] {url}")
    urllib.request.urlretrieve(url, dst)


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--repo", default=REPO_DEFAULT)
    ap.add_argument("--source-file", default=SOURCE_FILE)
    ap.add_argument("-o", "--out", default="lama.onnx")
    ap.add_argument("--size", type=int, default=RUNTIME_SIZE)
    ap.add_argument("--skip-fetch", action="store_true",
                    help="verify an already-downloaded --out instead of fetching")
    args = ap.parse_args()

    if args.size != RUNTIME_SIZE:
        sys.exit(
            f"--size {args.size} does not match the runtime's pinned "
            f"{RUNTIME_SIZE}. The graph's spatial dims are fixed and the "
            "shipped runtime would reject every inference."
        )

    lic = check_license(args.repo)
    print(f"[licence] {args.repo}: {lic} — OK")

    if not args.skip_fetch:
        fetch(args.repo, args.source_file, args.out)

    import numpy as np
    import onnxruntime as ort

    print(f"[verify] loading {args.out}")
    try:
        sess = ort.InferenceSession(args.out, providers=["CPUExecutionProvider"])
    except Exception as e:  # noqa: BLE001 — we want the reason in the message
        sys.exit(
            f"REFUSING to host {args.out}: it does not load in ONNX Runtime.\n"
            f"  {type(e).__name__}: {e}\n"
            "If you pointed --source-file at the repo's plainly-named "
            "'lama.onnx', that graph is broken (DFT shape inference); use "
            "lama_fp32.onnx."
        )

    ins = {i.name: i for i in sess.get_inputs()}
    outs = sess.get_outputs()
    print("  inputs :", [(i.name, i.shape) for i in sess.get_inputs()])
    print("  outputs:", [(o.name, o.shape) for o in outs])

    if len(ins) != 2 or len(outs) != 1:
        sys.exit(f"unexpected arity: {len(ins)} inputs / {len(outs)} outputs "
                 "(expected 2 in / 1 out).")
    for name in ("image", "mask"):
        if name not in ins:
            sys.exit(f"missing expected input '{name}'; got {sorted(ins)}.")
    for name, want_c in (("image", 3), ("mask", 1)):
        shape = ins[name].shape
        if list(shape[1:]) != [want_c, args.size, args.size]:
            sys.exit(f"input '{name}' shape {shape} does not match the pinned "
                     f"[batch,{want_c},{args.size},{args.size}].")

    S = args.size
    rng = np.random.default_rng(0)
    # A structured image, not noise: a flat field cannot tell a working
    # inpainter from one that returns its input.
    yy, xx = np.mgrid[0:S, 0:S]
    img = np.stack([
        (np.sin(xx / 24.0) * 0.5 + 0.5),
        (np.cos(yy / 31.0) * 0.5 + 0.5),
        ((xx + yy) % 64) / 64.0,
    ]).astype(np.float32)
    img = np.clip(img + rng.normal(0, 0.02, img.shape), 0, 1).astype(np.float32)

    mask = np.zeros((1, 1, S, S), np.float32)
    mask[:, :, S // 4:S // 4 + 40, S // 4:S // 4 + 40] = 1.0   # one square hole
    mask[:, :, :, S // 2:S // 2 + 3] = 1.0                     # one thin seam

    out = sess.run(None, {"image": img[None], "mask": mask})[0]

    if not np.isfinite(out).all():
        sys.exit("REFUSING to host: the graph emitted non-finite values "
                 "(this is exactly the #1025 failure mode).")

    lo, hi, mean = float(out.min()), float(out.max()), float(out.mean())
    print(f"[verify] output range [{lo:.2f}, {hi:.2f}] mean {mean:.2f}")
    # 0..255 output, NOT [0,1] — the asymmetry the C++ decoder depends on.
    if hi <= 1.5:
        sys.exit(f"output max {hi:.3f} looks like [0,1], but TextureInpaint "
                 "decodes 0..255. The contract changed — fix the C++ decoder.")
    if hi > 300.0:
        sys.exit(f"output max {hi:.3f} is out of the expected 0..255 range.")

    # The saturation trap: an UN-scaled image must NOT be what we feed. Prove
    # the distinction is real and measurable, so the C++ side's [0,1] scaling
    # is justified by evidence rather than by assumption.
    out_bad = sess.run(None, {"image": (img * 255.0)[None], "mask": mask})[0]
    # Saturation shows as a mean far above the correct one, not as a hard 255:
    # the measured values are ~247 (un-scaled) vs ~129 (correct).
    if float(out_bad.mean()) < mean + 60.0:
        print(f"[warn] un-scaled input gave mean {out_bad.mean():.1f}; the "
              "saturation trap may no longer apply to this graph.")
    else:
        print(f"[verify] saturation trap confirmed: un-scaled input → mean "
              f"{out_bad.mean():.1f} (correct input → {mean:.1f})")

    # Unmasked texels must survive: the C++ side composites anyway, but a graph
    # that mangles them would make the composite the ONLY thing holding quality.
    src255 = np.clip(img * 255.0, 0, 255)
    keep = mask[0, 0] == 0
    leak = float(np.abs(out[0].transpose(1, 2, 0)[keep]
                        - src255.transpose(1, 2, 0)[keep]).mean())
    print(f"[verify] unmasked-texel MAE (leakage): {leak:.4f}")
    if leak > 2.0:
        sys.exit(f"the graph altered unmasked texels (MAE {leak:.3f}); it is "
                 "not behaving as an inpainter.")

    # The masked region must actually CHANGE — a graph that copies its input
    # through would pass every check above.
    filled = mask[0, 0] > 0
    delta = float(np.abs(out[0].transpose(1, 2, 0)[filled]
                         - src255.transpose(1, 2, 0)[filled]).mean())
    print(f"[verify] masked-region change: {delta:.3f}")
    if delta < 1.0:
        sys.exit("the masked region is unchanged — the graph is a pass-through, "
                 "not an inpainter.")

    with open(args.out, "rb") as f:
        digest = hashlib.sha256(f.read()).hexdigest()
    print(f"\n[ok] {args.out} verified.")
    print(f"     sha256 {digest}")
    print("     Host it as 'lama.onnx' under pbr/ on the models repo, and "
          "register it in scripts/sync-hf-model-repos.sh.")


if __name__ == "__main__":
    main()
