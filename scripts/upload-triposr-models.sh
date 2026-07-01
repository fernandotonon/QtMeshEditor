#!/usr/bin/env bash
# Upload the image-to-3D models to the QtMeshEditor HF models repo (epic #764,
# slice #769 — model hosting). ONE-TIME, run by a maintainer with write access.
#
# The app downloads these on first use from
#   https://huggingface.co/fernandotonon/QtMeshEditor-models/resolve/main/{triposr,rembg}/...
# so the file names + subfolders below MUST match MeshGenPredictor::encoderFileName /
# decoderModelPath and BackgroundRemover's u2net.onnx.
#
# Prereqs:
#   pip install -U "huggingface_hub[cli]"
#   huggingface-cli login          # a token with write access to the repo
#   export-triposr-onnx.py already run → OUT_DIR holds the encoder(+fp16/int8)+decoder
#
# Usage:
#   OUT_DIR=/path/to/triposr_out  U2NET=/path/to/u2net.onnx  ./scripts/upload-triposr-models.sh
set -euo pipefail

REPO="${REPO:-fernandotonon/QtMeshEditor-models}"
OUT_DIR="${OUT_DIR:?set OUT_DIR to the export output dir (triposr_encoder*.onnx + triposr_decoder.onnx)}"
U2NET="${U2NET:-}"   # optional: path to u2net.onnx for the background remover

upload() {  # <local> <repo-path>
    local src="$1" dst="$2"
    if [ -f "$src" ]; then
        echo ">> uploading $src -> $REPO:$dst"
        huggingface-cli upload "$REPO" "$src" "$dst"
    else
        echo "!! skip (missing): $src"
    fi
}

# TripoSR encoder tiers + decoder (decoder is required; fp16/int8 optional).
upload "$OUT_DIR/triposr_encoder.onnx"      "triposr/triposr_encoder.onnx"
upload "$OUT_DIR/triposr_encoder_fp16.onnx" "triposr/triposr_encoder_fp16.onnx"
upload "$OUT_DIR/triposr_encoder_int8.onnx" "triposr/triposr_encoder_int8.onnx"
upload "$OUT_DIR/triposr_decoder.onnx"      "triposr/triposr_decoder.onnx"

# U²-Net background remover (rembg's model; Apache-2.0).
[ -n "$U2NET" ] && upload "$U2NET" "rembg/u2net.onnx"

echo "done. Verify: curl -sI https://huggingface.co/$REPO/resolve/main/triposr/triposr_decoder.onnx | head -1"
