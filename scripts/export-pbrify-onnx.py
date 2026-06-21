#!/usr/bin/env python3
"""Convert the CC0 PBRify_Remix SPAN models (.pth) to ONNX for #404.

This is a ONE-TIME, OFFLINE developer tool — it is NOT shipped with the app and
the app never runs Python. The app runs the resulting .onnx files in C++ via
ONNX Runtime (see src/PbrMapSynth.cpp / src/AIAssistManager.cpp).

Models (all CC0-1.0, trained on CC0 AmbientCG/Poly Haven textures):
  https://github.com/Kim2091/PBRify_Remix   (LICENSE: CC0-1.0)
    Models/1x-PBRify_NormalV3.pth     albedo -> tangent-space normal (RGB)
    Models/1x-PBRify_RoughnessV2.pth  albedo -> roughness (grayscale via RGB)
    Models/1x-PBRify_Height.pth       albedo -> height (grayscale via RGB)

All three are SPAN, scale 1x, 3-channel in -> 3-channel out. Output values can
exceed [0,1] and must be clamped by the consumer (PbrMapSynth does this).

Usage:
    python3 -m venv venv && ./venv/bin/pip install torch spandrel onnx onnxruntime onnxscript
    ./venv/bin/python scripts/export-pbrify-onnx.py --download --out-dir dist/pbr_onnx
Then host the .onnx files and point AIAssistManager's model URLs at them.
"""
import argparse
import os
import sys
import urllib.request

MODELS = {
    "normal":    "1x-PBRify_NormalV3",
    "roughness": "1x-PBRify_RoughnessV2",
    "height":    "1x-PBRify_Height",
}
# Pin to a specific commit (not the mutable `main`) so exports are reproducible
# and the source can't change under us. Bump deliberately when re-exporting.
PBRIFY_REF = "190db5378909749bdbad0f951b5724ba066ea32d"
BASE_URL = "https://github.com/Kim2091/PBRify_Remix/raw/" + PBRIFY_REF + "/Models/{name}.pth"


def download(name: str, dest: str) -> None:
    url = BASE_URL.format(name=name)
    print(f"  downloading {url}")
    urllib.request.urlretrieve(url, dest)


def export_one(pth_path: str, onnx_path: str) -> None:
    import torch
    from spandrel import ModelLoader
    import onnxruntime as ort
    import numpy as np

    desc = ModelLoader().load_from_file(pth_path)
    net = desc.model.eval()
    arch = getattr(getattr(desc, "architecture", None), "name", type(net).__name__)
    print(f"  arch={arch} scale={getattr(desc,'scale',None)} "
          f"in={getattr(desc,'input_channels',None)} out={getattr(desc,'output_channels',None)}")

    dummy = torch.rand(1, 3, 64, 64)
    # Legacy exporter (dynamo=False) + opset 18 is the recipe that exports SPAN
    # cleanly with dynamic H/W; the newer dynamo path rejects dynamic_axes here.
    torch.onnx.export(
        net, dummy, onnx_path, opset_version=18, dynamo=False,
        input_names=["input"], output_names=["output"],
        dynamic_axes={"input": {0: "b", 2: "h", 3: "w"},
                      "output": {0: "b", 2: "h", 3: "w"}})

    # Verify the artifact loads + runs in ONNX Runtime (CPU EP).
    sess = ort.InferenceSession(onnx_path, providers=["CPUExecutionProvider"])
    r = sess.run(None, {sess.get_inputs()[0].name: dummy.numpy()})[0]
    print(f"  -> {onnx_path} ({os.path.getsize(onnx_path)} bytes); "
          f"verified run {r.shape} range[{r.min():.3f},{r.max():.3f}]")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--download", action="store_true",
                    help="download the .pth models first")
    ap.add_argument("--pth-dir", default=".",
                    help="dir containing the .pth files (or download target)")
    ap.add_argument("--out-dir", default="pbr_onnx",
                    help="dir to write the .onnx files")
    args = ap.parse_args()

    os.makedirs(args.pth_dir, exist_ok=True)
    os.makedirs(args.out_dir, exist_ok=True)

    for slot, name in MODELS.items():
        print(f"=== {slot}: {name} ===")
        pth = os.path.join(args.pth_dir, name + ".pth")
        if args.download or not os.path.exists(pth):
            download(name, pth)
        export_one(pth, os.path.join(args.out_dir, name + ".onnx"))
    print("ALL EXPORTS OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
