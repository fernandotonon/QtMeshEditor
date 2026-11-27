#!/usr/bin/env python3
"""Export SAM 2.1 (Meta, Apache-2.0) to two ONNX graphs for generic mesh
segmentation (#1179, docs/GENERIC_SEGMENTATION_DESIGN.md).

Offline dev tool, not shipped. Needs the official repo installed
(`pip install -e sam2`, SAM2_BUILD_CUDA=0) and a checkpoint
(`sam2.1_hiera_small.pt` / `_tiny.pt`).

Graphs (the C++ side, GenericSeg::SamMaskProvider, mirrors this contract):

  <tier>_encoder.onnx
    in : image          [1,3,1024,1024] float, ImageNet mean/std normalised RGB
    out: image_embed    [1,256,64,64]   (no_mem_embed already added)
         high_res_feat0 [1,32,256,256]
         high_res_feat1 [1,64,128,128]

  <tier>_decoder.onnx
    in : image_embed, high_res_feat0, high_res_feat1 (as above)
         point_coords   [B,N,2] float, pixel coords in the 1024x1024 frame
         point_labels   [B,N]   float, 1 = foreground, 0 = background
    out: mask_logits    [B,3,256,256] low-res logits (threshold 0)
         iou_scores     [B,3]

The padding point the prompt encoder appends when there is no box, and the
not-a-point / label embeddings, are inside the decoder graph. The upstream
code writes them with indexed assignment (coords[:, :, 0] = ...), which the
TorchScript exporter turns into Add nodes with EMPTY inputs that only fail
when onnxruntime loads the graph (the SAM 3D Body lesson) - so the wrapper
re-expresses them functionally.

Gate (refuses to report success otherwise): both graphs load in onnxruntime,
outputs are finite, run with a different batch / point count than traced
(no frozen shapes), and match PyTorch on the given images: mask IoU >= 0.99
for every mask, IoU-score difference < 1e-3.

  python scripts/export-sam2-onnx.py --checkpoint sam2.1_hiera_small.pt \\
      --config configs/sam2.1/sam2.1_hiera_s.yaml --tier sam2.1_hiera_small \\
      --out out/ --image view0.png --image view1.png
"""
import argparse
import hashlib
import os
import sys

import numpy as np
import torch
from PIL import Image

MEAN = np.array([0.485, 0.456, 0.406], dtype=np.float32)
STD = np.array([0.229, 0.224, 0.225], dtype=np.float32)
SIZE = 1024


class Encoder(torch.nn.Module):
    def __init__(self, model):
        super().__init__()
        self.model = model

    def forward(self, image):
        backbone_out = self.model.forward_image(image)
        _, feats, _, sizes = self.model._prepare_backbone_features(backbone_out)
        feats[-1] = feats[-1] + self.model.no_mem_embed
        out = [f.permute(1, 2, 0).reshape(1, -1, s[0], s[1]) for f, s in zip(feats, sizes)]
        return out[2], out[0], out[1]


class Decoder(torch.nn.Module):
    def __init__(self, model):
        super().__init__()
        pe = model.sam_prompt_encoder
        self.decoder = model.sam_mask_decoder
        self.register_buffer("gauss", pe.pe_layer.positional_encoding_gaussian_matrix.detach().clone())
        self.register_buffer("image_pe", pe.get_dense_pe().detach().clone())
        self.register_buffer("not_a_point", pe.not_a_point_embed.weight.detach().clone())
        self.register_buffer("label_embed", torch.cat(
            [pe.point_embeddings[i].weight.detach() for i in range(4)], 0))
        self.register_buffer("no_mask", pe.no_mask_embed.weight.detach().reshape(1, -1, 1, 1).clone())
        self.grid = pe.image_embedding_size

    def forward(self, image_embed, high_res_feat0, high_res_feat1, point_coords, point_labels):
        b = point_coords.shape[0]
        pts = point_coords + 0.5
        pts = torch.cat([pts, torch.zeros_like(pts[:, :1, :])], dim=1)
        labels = torch.cat([point_labels, -torch.ones_like(point_labels[:, :1])], dim=1)
        c = 2.0 * (pts / float(SIZE)) - 1.0
        c = 2.0 * np.pi * (c @ self.gauss)
        emb = torch.cat([torch.sin(c), torch.cos(c)], dim=-1)
        lab = labels.unsqueeze(-1)
        emb = torch.where(lab == -1, torch.zeros_like(emb) + self.not_a_point, emb)
        emb = torch.where(lab == 0, emb + self.label_embed[0], emb)
        emb = torch.where(lab == 1, emb + self.label_embed[1], emb)
        dense = self.no_mask.expand(b, -1, self.grid[0], self.grid[1])
        masks, iou, _, _ = self.decoder(
            image_embeddings=image_embed, image_pe=self.image_pe,
            sparse_prompt_embeddings=emb, dense_prompt_embeddings=dense,
            multimask_output=True, repeat_image=True,
            high_res_features=[high_res_feat0, high_res_feat1])
        return masks, iou


def preprocess(path):
    img = Image.open(path).convert("RGB").resize((SIZE, SIZE), Image.BILINEAR)
    a = (np.asarray(img, dtype=np.float32) / 255.0 - MEAN) / STD
    return a.transpose(2, 0, 1)[None].astype(np.float32)


def grid_points(n, size=SIZE):
    step = size / n
    xs = (np.arange(n) + 0.5) * step
    g = np.stack(np.meshgrid(xs, xs), -1).reshape(-1, 1, 2).astype(np.float32)
    return g, np.ones((g.shape[0], 1), np.float32)


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--checkpoint", required=True)
    ap.add_argument("--config", required=True)
    ap.add_argument("--tier", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--image", action="append", default=[])
    ap.add_argument("--opset", type=int, default=17)
    ap.add_argument("--skip-export", action="store_true")
    args = ap.parse_args()

    from sam2.build_sam import build_sam2
    from sam2.sam2_image_predictor import SAM2ImagePredictor
    import onnxruntime as ort

    model = build_sam2(args.config, args.checkpoint, device="cpu").eval()
    enc, dec = Encoder(model).eval(), Decoder(model).eval()
    os.makedirs(args.out, exist_ok=True)
    enc_path = os.path.join(args.out, args.tier + "_encoder.onnx")
    dec_path = os.path.join(args.out, args.tier + "_decoder.onnx")

    dummy = torch.randn(1, 3, SIZE, SIZE)
    with torch.no_grad():
        e, f0, f1 = enc(dummy)
        if not args.skip_export:
            torch.onnx.export(enc, (dummy,), enc_path, opset_version=args.opset, dynamo=False,
                              input_names=["image"],
                              output_names=["image_embed", "high_res_feat0", "high_res_feat1"])
            pc = torch.rand(4, 2, 2) * SIZE
            pl = torch.tensor([[1.0, 0.0]] * 4)
            torch.onnx.export(dec, (e, f0, f1, pc, pl), dec_path, opset_version=args.opset, dynamo=False,
                              input_names=["image_embed", "high_res_feat0", "high_res_feat1",
                                           "point_coords", "point_labels"],
                              output_names=["mask_logits", "iou_scores"],
                              dynamic_axes={"point_coords": {0: "B", 1: "N"}, "point_labels": {0: "B", 1: "N"},
                                            "mask_logits": {0: "B"}, "iou_scores": {0: "B"}})

    so = ort.SessionOptions()
    se = ort.InferenceSession(enc_path, so, providers=["CPUExecutionProvider"])
    sd = ort.InferenceSession(dec_path, so, providers=["CPUExecutionProvider"])
    predictor = SAM2ImagePredictor(model)

    ok = True
    images = args.image or [None]
    for path in images:
        x = preprocess(path) if path else np.random.RandomState(0).randn(1, 3, SIZE, SIZE).astype(np.float32)
        oe, of0, of1 = se.run(None, {"image": x})
        for name, t in (("image_embed", oe), ("feat0", of0), ("feat1", of1)):
            if not np.isfinite(t).all():
                print(f"FAIL non-finite encoder output {name}")
                ok = False
        with torch.no_grad():
            te, tf0, tf1 = enc(torch.from_numpy(x))
        enc_diff = max(float(np.abs(oe - te.numpy()).max()), float(np.abs(of0 - tf0.numpy()).max()),
                       float(np.abs(of1 - tf1.numpy()).max()))

        # Reference: the upstream predictor on the same image (its own preprocessing).
        if path:
            predictor.set_image(np.asarray(Image.open(path).convert("RGB").resize((SIZE, SIZE), Image.BILINEAR)))
        else:
            predictor._features = {"image_embed": te, "high_res_feats": [tf0, tf1]}
            predictor._is_image_set = True
            predictor._orig_hw = [(SIZE, SIZE)]
        pts, labs = grid_points(8)
        om, oi = sd.run(None, {"image_embed": oe, "high_res_feat0": of0, "high_res_feat1": of1,
                               "point_coords": pts, "point_labels": labs})
        with torch.no_grad():
            _, ti, tm = predictor._predict(torch.from_numpy(pts), torch.from_numpy(labs),
                                           multimask_output=True, return_logits=True)
        tm, ti = tm.numpy(), ti.numpy()
        a, b = om > 0, tm > 0
        inter = (a & b).reshape(-1, 256 * 256).sum(1)
        union = (a | b).reshape(-1, 256 * 256).sum(1)
        ious = np.where(union > 0, inter / np.maximum(union, 1), 1.0)
        idiff = float(np.abs(oi - ti).max())
        print(f"{path or 'random'}: encoder max diff {enc_diff:.2e}, mask IoU min {ious.min():.4f}, "
              f"iou-score diff {idiff:.2e}, finite {np.isfinite(om).all()}")
        if ious.min() < 0.99 or idiff >= 1e-3 or not np.isfinite(om).all():
            ok = False

    # A shape the trace never saw: one prompt with three points.
    om, _ = sd.run(None, {"image_embed": oe, "high_res_feat0": of0, "high_res_feat1": of1,
                          "point_coords": np.array([[[100, 200], [300, 400], [500, 600]]], np.float32),
                          "point_labels": np.array([[1, 1, 0]], np.float32)})
    if om.shape != (1, 3, 256, 256) or not np.isfinite(om).all():
        print("FAIL dynamic shape", om.shape)
        ok = False

    for p in (enc_path, dec_path):
        print(f"{os.path.basename(p)}  {os.path.getsize(p)} bytes  sha256 {sha256(p)}")
    print("GATE PASSED" if ok else "GATE FAILED")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
