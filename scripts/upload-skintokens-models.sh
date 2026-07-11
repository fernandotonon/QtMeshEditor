#!/usr/bin/env bash
# Upload the SkinTokens ML-skinning models to the QtMeshEditor HF models repo
# (issue #819 Slice C follow-up). ONE-TIME, run by a maintainer with write
# access.
#
# The app downloads these on first use from
#   https://huggingface.co/fernandotonon/QtMeshEditor-models/resolve/main/skintokens/...
# so the file names below MUST match src/SkinTokensPredictor.cpp's kFiles.
#
# Prereqs:
#   pip install -U "huggingface_hub[cli]"
#   huggingface-cli login          # a token with write access to the repo
#   export-skintokens-onnx.py already run → OUT_DIR holds the five graphs +
#   decoder.onnx.data + skintokens.json
#
# Usage:
#   OUT_DIR=/path/to/skintokens_onnx ./scripts/upload-skintokens-models.sh
set -euo pipefail

REPO="${REPO:-fernandotonon/QtMeshEditor-models}"
OUT_DIR="${OUT_DIR:?set OUT_DIR to the export output dir}"

FILES=(
    skintokens.json
    mesh_cond.onnx
    vae_cond.onnx
    embed.onnx
    decoder.onnx
    decoder.onnx.data
    skin_decode.onnx
)

for f in "${FILES[@]}"; do
    [ -f "$OUT_DIR/$f" ] || { echo "missing $OUT_DIR/$f"; exit 1; }
done

for f in "${FILES[@]}"; do
    echo "uploading skintokens/$f …"
    huggingface-cli upload "$REPO" "$OUT_DIR/$f" "skintokens/$f" --repo-type model
done

echo "done — verify at https://huggingface.co/$REPO/tree/main/skintokens"
