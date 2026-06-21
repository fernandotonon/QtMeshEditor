#!/usr/bin/env python3
"""Convert the BSD-3 Real-ESRGAN weights (.pth) to ONNX for #405.

ONE-TIME, OFFLINE dev tool — NOT shipped; the app runs the resulting .onnx in
C++ via ONNX Runtime (src/TextureUpscaler.cpp). Mirrors export-pbrify-onnx.py.

Models (BSD-3-Clause, Xintao Wang — https://github.com/xinntao/Real-ESRGAN,
LICENSE has no code/weights carve-out):
    RealESRGAN_x4plus.pth   4x super-resolution (RRDBNet)
    RealESRGAN_x2plus.pth   2x super-resolution (RRDBNet)
Both are 3-channel in -> 3-channel out, output H*scale x W*scale, values [0,1].

Usage:
    python3 -m venv venv
    ./venv/bin/pip install torch spandrel onnx onnxruntime onnxscript
    ./venv/bin/python scripts/export-realesrgan-onnx.py --download --out-dir dist/esrgan_onnx
Then host the .onnx files and point AIAssistManager's model base URL at them.
"""
import argparse
import os
import sys
import urllib.request

# (filename-stem, release tag) — pinned release assets (immutable).
MODELS = {
    "x4": ("RealESRGAN_x4plus", "v0.1.0"),
    "x2": ("RealESRGAN_x2plus", "v0.2.1"),
}
BASE_URL = "https://github.com/xinntao/Real-ESRGAN/releases/download/{tag}/{name}.pth"


def download(name: str, tag: str, dest: str) -> None:
    url = BASE_URL.format(tag=tag, name=name)
    print(f"  downloading {url}")
    urllib.request.urlretrieve(url, dest)


def export_one(pth_path: str, onnx_path: str) -> None:
    import torch
    from spandrel import ModelLoader
    import onnxruntime as ort

    desc = ModelLoader().load_from_file(pth_path)
    net = desc.model.eval()
    print(f"  arch={getattr(getattr(desc,'architecture',None),'name','?')} "
          f"scale={getattr(desc,'scale',None)}")

    dummy = torch.rand(1, 3, 64, 64)
    torch.onnx.export(
        net, dummy, onnx_path, opset_version=18, dynamo=False,
        input_names=["input"], output_names=["output"],
        dynamic_axes={"input": {0: "b", 2: "h", 3: "w"},
                      "output": {0: "b", 2: "h", 3: "w"}})

    sess = ort.InferenceSession(onnx_path, providers=["CPUExecutionProvider"])
    r = sess.run(None, {sess.get_inputs()[0].name: dummy.numpy()})[0]
    print(f"  -> {onnx_path} ({os.path.getsize(onnx_path)} bytes); "
          f"64x64 -> {r.shape[2]}x{r.shape[3]} ({r.shape[2] // 64}x)")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--download", action="store_true")
    ap.add_argument("--pth-dir", default=".")
    ap.add_argument("--out-dir", default="esrgan_onnx")
    args = ap.parse_args()

    os.makedirs(args.pth_dir, exist_ok=True)
    os.makedirs(args.out_dir, exist_ok=True)

    for key, (name, tag) in MODELS.items():
        print(f"=== {key}: {name} ===")
        pth = os.path.join(args.pth_dir, name + ".pth")
        if args.download or not os.path.exists(pth):
            download(name, tag, pth)
        export_one(pth, os.path.join(args.out_dir, name + ".onnx"))
    print("ALL EXPORTS OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
