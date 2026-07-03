#!/usr/bin/env bash
# Upload the TripoSG ONNX graphs to the QtMeshEditor HF models repo (image-to-3D
# backend #2). ONE-TIME, run by a maintainer with write access.
#
# The app downloads these on first use from
#   https://huggingface.co/fernandotonon/QtMeshEditor-models/resolve/main/triposg/...
# so the file names below MUST match TripoSGPredictor's kImageEncoderFile /
# kDitStepFile (+ .onnx.data sidecar!) / kDitStepInt8File / kVaeLatentsFile /
# kVaeDecoderFile.
#
# Prereqs:
#   pip install -U "huggingface_hub[cli]"     (or reuse /tmp/triposg_export/venv)
#   huggingface-cli login                     # token with write access
#   scripts/run-triposg-export.sh already run → OUT_DIR holds the graphs
#
# Usage:
#   OUT_DIR=/tmp/triposg_export/dist/triposg_onnx ./scripts/upload-triposg-models.sh
set -euo pipefail

REPO="${REPO:-fernandotonon/QtMeshEditor-models}"
OUT_DIR="${OUT_DIR:-/tmp/triposg_export/dist/triposg_onnx}"
HFCLI="${HFCLI:-huggingface-cli}"
command -v "$HFCLI" >/dev/null 2>&1 || HFCLI=/tmp/triposg_export/venv/bin/huggingface-cli

upload() {  # <local> <repo-path>
    local src="$1" dst="$2"
    if [ -f "$src" ]; then
        echo ">> uploading $src -> $REPO:$dst"
        "$HFCLI" upload "$REPO" "$src" "$dst"
    else
        echo "!! skip (missing): $src"
    fi
}

# The fp32 DiT is a graph + external-weights sidecar: BOTH must be hosted
# side-by-side or ONNX Runtime cannot load the graph.
upload "$OUT_DIR/triposg_image_encoder.onnx"  "triposg/triposg_image_encoder.onnx"
upload "$OUT_DIR/triposg_dit_step.onnx"       "triposg/triposg_dit_step.onnx"
upload "$OUT_DIR/triposg_dit_step.onnx.data"  "triposg/triposg_dit_step.onnx.data"
upload "$OUT_DIR/triposg_dit_step_int8.onnx"  "triposg/triposg_dit_step_int8.onnx"
upload "$OUT_DIR/triposg_vae_latents.onnx"    "triposg/triposg_vae_latents.onnx"
upload "$OUT_DIR/triposg_vae_decoder.onnx"    "triposg/triposg_vae_decoder.onnx"

echo "done. Verify: curl -sI https://huggingface.co/$REPO/resolve/main/triposg/triposg_vae_decoder.onnx | head -1"
