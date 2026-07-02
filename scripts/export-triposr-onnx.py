#!/usr/bin/env python3
"""Export the TripoSR single-image-to-3D network to ONNX (epic #764, slice A #765).

ONE-TIME, OFFLINE developer tool — NOT shipped with the app, NOT wired into CMake
or CI. The app never runs Python. It runs the exported .onnx files in C++ via ONNX
Runtime (src/MeshGenPredictor.cpp, slice B #766), downloading them on first use to
AppData/ai_models/triposr/.

WHY A SPIKE
  TripoSR (VAST-AI-Research/TripoSR, Tripo AI + Stability AI — MIT code AND MIT
  weights, HF `stabilityai/TripoSR`) ships no official .onnx, and its pipeline is
  NOT one clean tensor-in/mesh-out graph:
      image -> DINO ViT tokenizer -> triplane transformer backbone
            -> post_processor -> scene_codes (triplane)          [ENCODER]
      scene_codes + query points -> grid_sample + NeRF MLP -> density(+color)  [DECODER]
      density grid -> marching cubes -> mesh                     [HOST-SIDE C++]
  This script proves the two network stages export to ONNX and records the exact
  tensor contract; the host-side marching cubes is native C++ (src/MarchingCubes.*).

  License rationale: MIT code + MIT weights clears QtMeshEditor's permissive-
  redistribution bar (Homebrew/Snap/WinGet/Docker) — the same reason UniRig (#408)
  passed and non-commercial SF3D/Stable-Fast-3D was rejected. See
  THIRD_PARTY_AI_MODELS.md.

WHAT IT PRODUCES  (the contract MeshGenPredictor::predict() will target)
  triposr_encoder.onnx
    input  "image"       float32 [1, 3, S, S]   S = cfg.cond_image_size (512), RGB in [0,1]
                                                 (plain /255 — NO ImageNet mean/std)
    output "scene_codes" float32 [1, 3, Ct, Ht, Wt]   the triplane (3 planes)
  triposr_decoder.onnx
    input  "scene_codes" float32 [1, 3, Ct, Ht, Wt]
    input  "points"      float32 [1, P, 3]      query points in world coords (-radius..radius)
    output "density"     float32 [1, P, 1]      pre-threshold density (post density_act)
    output "color"       float32 [1, P, 3]      vertex color (sigmoid features), optional

  The decoder is a per-point graph so C++ tiles the resolution^3 grid through it in
  chunks (grid vertices in [0,1]^3 scaled to (-radius,radius); see extract_mesh /
  query_triplane in tsr/system.py). Marching cubes runs on `density - threshold`
  at iso 0 (threshold default 25.0), i.e. our native MarchingCubes::extract(field=
  density-threshold, isoLevel=0) — see src/MarchingCubes.h. (TripoSR's own
  isosurface uses `-(density - threshold)` because its MC treats the LOW side as
  inside; ours is inside-positive, so the sign flips — same surface.)

GRID / AXIS NOTE (for slice B/C)
  TripoSR's MarchingCubeHelper builds grid vertices with meshgrid(x,y,z,
  indexing="ij") reshaped to [-1,3] (x slowest, z fastest) and swaps output axes
  [2,1,0], dividing by (resolution-1). Our native MC uses row-major
  field[z*ny*nx + y*nx + x] (x fastest). Slice B must fill the density grid in the
  order the decoder is queried and hand MC a consistent layout; this script prints
  the grid_vertices ordering so the C++ side matches.

USAGE (offline venv with torch + transformers + onnx + omegaconf + einops + trimesh):
    pip install torch torchvision transformers einops omegaconf onnx onnxruntime pillow
    git clone https://github.com/VAST-AI-Research/TripoSR
    python export-triposr-onnx.py --triposr ./TripoSR --out ./out [--resolution 256]

This file is kept for reproducibility; hosting the exported .onnx on the HF models
repo is slice E (#769). Until hosted, MeshGenPredictor reports a clean
"TripoSR model not yet hosted" state (the RigNet precedent).
"""
import argparse
import os
import sys

import numpy as np
import torch


def load_triposr(triposr_dir):
    """Import the tsr package from a TripoSR checkout and build the model from the
    HF weights. Returns (model, cond_image_size, radius)."""
    sys.path.insert(0, triposr_dir)
    from huggingface_hub import hf_hub_download
    from tsr.system import TSR

    config_path = hf_hub_download("stabilityai/TripoSR", "config.yaml")
    weight_path = hf_hub_download("stabilityai/TripoSR", "model.ckpt")
    model = TSR.from_pretrained(
        "stabilityai/TripoSR",
        config_name="config.yaml",
        weight_name="model.ckpt",
    )
    model.eval()
    cond = int(model.cfg.cond_image_size)
    radius = float(model.renderer.cfg.radius)
    return model, cond, radius


class EncoderWrapper(torch.nn.Module):
    """image [1,3,S,S] (RGB, [0,1]) -> scene_codes triplane. Reproduces
    TSR.forward up to scene_codes without the PIL ImagePreprocessor (the C++ side
    feeds an already-resized [0,1] NCHW tensor)."""

    def __init__(self, m, cond_image_size):
        super().__init__()
        self.image_tokenizer = m.image_tokenizer
        self.tokenizer = m.tokenizer
        self.backbone = m.backbone
        self.post_processor = m.post_processor
        self._freeze_vit_pos_encoding(cond_image_size)

    def _freeze_vit_pos_encoding(self, cond_image_size):
        """The DINO ViT interpolates its positional embedding from 224 → 512 at
        runtime via nn.functional.interpolate(bicubic), which does NOT trace to
        ONNX (upsample_bicubic2d rejects the traced dynamic output_size — the
        classic ViT-at-non-native-resolution export failure). Because the input
        size is FIXED (cond_image_size), the interpolated table is a CONSTANT:
        precompute it once in eager mode and replace interpolate_pos_encoding
        with a lambda returning that frozen tensor, so the trace sees a Constant
        instead of an untraceable interpolate."""
        import torch as _t

        vit = self.image_tokenizer.model
        emb = vit.embeddings
        with _t.no_grad():
            dummy = _t.zeros(1, 3, cond_image_size, cond_image_size)
            patch = emb.patch_embeddings(dummy, interpolate_pos_encoding=True)
            seq = _t.cat([emb.cls_token.expand(1, -1, -1), patch], dim=1)
            frozen = emb.interpolate_pos_encoding(seq, cond_image_size, cond_image_size).detach()
        emb.interpolate_pos_encoding = lambda embeddings, height, width: frozen

    def forward(self, image):
        from einops import rearrange

        # image: [B,3,S,S] -> tokenizer expects [B, Nv, C, H, W] with Nv=1.
        tokens_in = image[:, None]  # [B,1,3,S,S]
        img_tokens = self.image_tokenizer(tokens_in)
        img_tokens = rearrange(img_tokens, "B Nv C Nt -> B (Nv Nt) C", Nv=1)
        tokens = self.tokenizer(image.shape[0])
        tokens = self.backbone(tokens, encoder_hidden_states=img_tokens)
        scene_codes = self.post_processor(self.tokenizer.detokenize(tokens))
        return scene_codes


class DecoderWrapper(torch.nn.Module):
    """scene_codes + points [1,P,3] -> density [1,P,1], color [1,P,3].
    Reproduces renderer.query_triplane for one triplane (no chunking — the C++
    caller chunks)."""

    def __init__(self, m):
        super().__init__()
        self.renderer = m.renderer
        self.decoder = m.decoder

    def forward(self, scene_codes, points):
        # scene_codes: [1,3,Ct,Ht,Wt]; points: [1,P,3]
        out = self.renderer.query_triplane(self.decoder, points[0], scene_codes[0])
        density = out["density_act"].reshape(1, -1, 1)
        color = out["color"].reshape(1, -1, 3)
        return density, color


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--triposr", required=True, help="path to a TripoSR git checkout")
    ap.add_argument("--out", default="./out", help="output dir for the .onnx files")
    ap.add_argument("--resolution", type=int, default=256, help="probe grid res (contract only)")
    ap.add_argument("--opset", type=int, default=17)
    ap.add_argument("--verify", action="store_true", help="run onnxruntime shape check")
    ap.add_argument("--no-quant", action="store_true",
                    help="skip the fp16/int8 quantized encoder variants (export fp32 only)")
    args = ap.parse_args()

    os.makedirs(args.out, exist_ok=True)
    model, cond, radius = load_triposr(args.triposr)
    print(f"[contract] cond_image_size={cond}  renderer.radius={radius}")

    enc = EncoderWrapper(model, cond).eval()
    dec = DecoderWrapper(model).eval()

    # ---- Encoder export ------------------------------------------------------
    dummy_img = torch.rand(1, 3, cond, cond, dtype=torch.float32)
    with torch.no_grad():
        scene_codes = enc(dummy_img)
    print(f"[contract] scene_codes shape={tuple(scene_codes.shape)} dtype={scene_codes.dtype}")

    enc_path = os.path.join(args.out, "triposr_encoder.onnx")
    torch.onnx.export(
        enc, (dummy_img,), enc_path,
        input_names=["image"], output_names=["scene_codes"],
        dynamic_axes={"image": {0: "B"}, "scene_codes": {0: "B"}},
        opset_version=args.opset, do_constant_folding=True,
        dynamo=False,   # legacy TorchScript exporter — no onnxscript dependency
    )
    print(f"[ok] wrote {enc_path}")

    # ---- Quantized encoder tier (int8) ---------------------------------------
    # The ~1.68 GB fp32 encoder dominates the first-use download, so also emit an
    # int8 variant (~quarter size, slight quality loss) via onnxruntime dynamic
    # quantization. The decoder stays fp32 (tiny). File name MUST match
    # MeshGenPredictor::encoderFileName(): triposr_encoder_int8.onnx.
    #
    # NOTE: fp16 was intentionally NOT emitted — TripoSR's attention blocks contain
    # a hardcoded Cast-to-float32 whose output type the ONNX fp16 converters
    # (onnxconverter_common float16 / auto_convert_mixed_precision) can't rewrite,
    # producing a model that fails to load in ONNX Runtime. int8 is smaller anyway.
    if not args.no_quant:
        try:
            from onnxruntime.quantization import quantize_dynamic, QuantType
            int8_path = os.path.join(args.out, "triposr_encoder_int8.onnx")
            # MatMul-only: leaving Conv unquantized avoids ConvInteger, which our
            # ONNX Runtime CPU EP has no kernel for (the ViT patch-embed Conv would
            # otherwise fail inference: "Could not find an implementation for
            # ConvInteger"). ViT weight is MatMul-heavy so it still shrinks ~4x.
            quantize_dynamic(enc_path, int8_path, weight_type=QuantType.QInt8,
                             op_types_to_quantize=['MatMul'])
            print(f"[ok] wrote {int8_path}")
        except Exception as e:  # noqa: BLE001 — best-effort; fp32 still ships
            print(f"[warn] int8 export skipped: {e}")

    # ---- Decoder export ------------------------------------------------------
    P = 4096
    dummy_pts = (torch.rand(1, P, 3, dtype=torch.float32) * 2 - 1) * radius
    with torch.no_grad():
        d, c = dec(scene_codes, dummy_pts)
    print(f"[contract] density shape={tuple(d.shape)} color shape={tuple(c.shape)}")

    dec_path = os.path.join(args.out, "triposr_decoder.onnx")
    torch.onnx.export(
        dec, (scene_codes, dummy_pts), dec_path,
        input_names=["scene_codes", "points"], output_names=["density", "color"],
        dynamic_axes={"points": {1: "P"}, "density": {1: "P"}, "color": {1: "P"}},
        opset_version=args.opset, do_constant_folding=True,
        dynamo=False,   # legacy TorchScript exporter — no onnxscript dependency
    )
    print(f"[ok] wrote {dec_path}")

    # ---- Optional ORT verification ------------------------------------------
    if args.verify:
        import onnxruntime as ort

        se = ort.InferenceSession(enc_path, providers=["CPUExecutionProvider"])
        sc = se.run(None, {"image": dummy_img.numpy()})[0]
        print(f"[verify] ORT encoder scene_codes={sc.shape}  "
              f"match={np.allclose(sc, scene_codes.numpy(), atol=1e-3)}")
        sd = ort.InferenceSession(dec_path, providers=["CPUExecutionProvider"])
        dd, cc = sd.run(None, {"scene_codes": sc, "points": dummy_pts.numpy()})
        print(f"[verify] ORT decoder density={dd.shape} color={cc.shape}  "
              f"match={np.allclose(dd, d.numpy(), atol=1e-2)}")
    print("[done] export spike complete.")


if __name__ == "__main__":
    main()
