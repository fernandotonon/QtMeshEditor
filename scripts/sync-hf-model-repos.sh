#!/bin/bash
# Sync the dedicated per-model Hugging Face repos from the aggregate
# fernandotonon/QtMeshEditor-models repo (the one the app downloads from).
#
# The aggregate repo is the SOURCE OF TRUTH — after re-exporting a model and
# uploading it there (upload-skintokens-models.sh, upload-triposr-models.sh,
# …), run this to refresh the standalone mirrors:
#
#   ./scripts/sync-hf-model-repos.sh              # sync every mirror
#   ./scripts/sync-hf-model-repos.sh skintokens   # sync one mirror
#
# Each dedicated repo carries its own model card (README.md), which this
# script deliberately does NOT touch — edit cards on the hub or via
# `huggingface-cli upload <repo> README.md`.
#
# Mirrors (created 2026-07-10; -onnx suffix = converted third-party weights):
#   pbrify     → QtMeshEditor-pbrify-onnx      (CC0,  Kim2091/PBRify_Remix)
#   unirig     → QtMeshEditor-unirig-onnx      (MIT,  VAST-AI UniRig skeleton stage)
#   skintokens → QtMeshEditor-skintokens-onnx  (MIT,  VAST-AI SkinTokens/TokenRig)
#   triposr    → QtMeshEditor-triposr-onnx     (MIT,  Stability/Tripo TripoSR)
#   triposg    → QtMeshEditor-triposg-onnx     (MIT,  VAST-AI TripoSG; int8 tier NOT mirrored — deprecated)
# In-house models have their own repos already (rmib-inbetween,
# mesh-segmentation, t2m) with their own upload flows.
set -euo pipefail

OWNER=fernandotonon
AGG=$OWNER/QtMeshEditor-models
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

declare -A FILES=(
  [pbrify]="1x-PBRify_NormalV3.onnx 1x-PBRify_RoughnessV2.onnx 1x-PBRify_Height.onnx"
  [unirig]="unirig/encoder.onnx unirig/decoder.onnx unirig/embed.onnx"
  [skintokens]="skintokens/skintokens.json skintokens/mesh_cond.onnx skintokens/vae_cond.onnx skintokens/embed.onnx skintokens/skin_decode.onnx skintokens/decoder.onnx skintokens/decoder.onnx.data"
  [triposr]="triposr/triposr_encoder.onnx triposr/triposr_encoder_int8.onnx triposr/triposr_decoder.onnx"
  [triposg]="triposg/triposg_image_encoder.onnx triposg/triposg_dit_step.onnx triposg/triposg_dit_step.onnx.data triposg/triposg_vae_latents.onnx triposg/triposg_vae_decoder.onnx"
)

sync_one() {
  local model=$1 repo="$OWNER/QtMeshEditor-$1-onnx"
  echo "=== $model → $repo"
  for f in ${FILES[$model]}; do
    echo "  $f"
    huggingface-cli download "$AGG" "$f" --local-dir "$WORK" --quiet
    huggingface-cli upload "$repo" "$WORK/$f" "$(basename "$f")" --quiet
    rm -f "$WORK/$f"          # multi-GB files — don't accumulate
  done
}

if [ $# -gt 0 ]; then
  for m in "$@"; do sync_one "$m"; done
else
  for m in "${!FILES[@]}"; do sync_one "$m"; done
fi
echo "done"
