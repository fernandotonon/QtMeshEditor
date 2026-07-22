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
# Mirrors (created 2026-07-10; -onnx/-gguf suffix = converted third-party
# weights; the in-house rmib-inbetween / mesh-segmentation / t2m repos have
# their own upload flows, but the t2m repo also mirrors motion-library.json):
#   pbrify     → QtMeshEditor-pbrify-onnx      (CC0,    Kim2091/PBRify_Remix)
#   realesrgan → QtMeshEditor-realesrgan-onnx  (BSD-3,  xinntao Real-ESRGAN)
#   unirig     → QtMeshEditor-unirig-onnx      (MIT,    VAST-AI UniRig skeleton stage)
#   skintokens → QtMeshEditor-skintokens-onnx  (MIT,    VAST-AI SkinTokens/TokenRig)
#   triposr    → QtMeshEditor-triposr-onnx     (MIT,    Stability/Tripo TripoSR)
#   triposg    → QtMeshEditor-triposg-onnx     (MIT,    VAST-AI TripoSG; int8 tier NOT mirrored — deprecated)
#   u2net      → QtMeshEditor-u2net-onnx       (Apache, U²-Net / rembg)
#   smolvlm    → QtMeshEditor-smolvlm-gguf     (Apache, HuggingFaceTB SmolVLM Q8_0)
#   motion     → QtMeshEditor-t2m              (CC0,    in-house; template clip library)
#   segveg     → QtMeshEditor-mesh-segmentation-vegetation (CC-BY-4.0, in-house #818 B2)
#   segveh     → QtMeshEditor-mesh-segmentation-vehicle    (CC-BY-4.0, in-house #818 B2)
#   segbld     → QtMeshEditor-mesh-segmentation-building   (CC-BY-4.0, in-house #818 B2)
#   segcls     → QtMeshEditor-mesh-segmentation-category   (CC-BY-4.0, in-house #818 B2 Auto dispatcher)
#   mocap-* (5, #869; Apache, converted Google MediaPipe graphs):
#     blazeface       → QtMeshEditor-blazeface-onnx       (face detector)
#     facemesh        → QtMeshEditor-facemesh-onnx        (478 face landmarks)
#     faceblendshapes → QtMeshEditor-faceblendshapes-onnx (52 ARKit blendshapes)
#     blazepose       → QtMeshEditor-blazepose-onnx       (person detector)
#     poselandmarks   → QtMeshEditor-poselandmarks-onnx   (33 world landmarks)
set -euo pipefail

OWNER=fernandotonon
AGG=$OWNER/QtMeshEditor-models
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

declare -A REPOS=(
  [pbrify]=QtMeshEditor-pbrify-onnx
  [realesrgan]=QtMeshEditor-realesrgan-onnx
  [unirig]=QtMeshEditor-unirig-onnx
  [skintokens]=QtMeshEditor-skintokens-onnx
  [triposr]=QtMeshEditor-triposr-onnx
  [triposg]=QtMeshEditor-triposg-onnx
  [u2net]=QtMeshEditor-u2net-onnx
  [smolvlm]=QtMeshEditor-smolvlm-gguf
  [motion]=QtMeshEditor-t2m
  [segveg]=QtMeshEditor-mesh-segmentation-vegetation
  [segveh]=QtMeshEditor-mesh-segmentation-vehicle
  [segbld]=QtMeshEditor-mesh-segmentation-building
  [segcls]=QtMeshEditor-mesh-segmentation-category
  [blazeface]=QtMeshEditor-blazeface-onnx
  [facemesh]=QtMeshEditor-facemesh-onnx
  [faceblendshapes]=QtMeshEditor-faceblendshapes-onnx
  [blazepose]=QtMeshEditor-blazepose-onnx
  [poselandmarks]=QtMeshEditor-poselandmarks-onnx
)
declare -A FILES=(
  [pbrify]="1x-PBRify_NormalV3.onnx 1x-PBRify_RoughnessV2.onnx 1x-PBRify_Height.onnx"
  [realesrgan]="RealESRGAN_x2plus.onnx RealESRGAN_x4plus.onnx"
  [unirig]="unirig/encoder.onnx unirig/decoder.onnx unirig/embed.onnx"
  [skintokens]="skintokens/skintokens.json skintokens/mesh_cond.onnx skintokens/vae_cond.onnx skintokens/embed.onnx skintokens/skin_decode.onnx skintokens/decoder.onnx skintokens/decoder.onnx.data"
  [triposr]="triposr/triposr_encoder.onnx triposr/triposr_encoder_int8.onnx triposr/triposr_decoder.onnx"
  [triposg]="triposg/triposg_image_encoder.onnx triposg/triposg_dit_step.onnx triposg/triposg_dit_step.onnx.data triposg/triposg_vae_latents.onnx triposg/triposg_vae_decoder.onnx"
  [u2net]="rembg/u2net.onnx"
  [smolvlm]="caption/SmolVLM-500M-Instruct-Q8_0.gguf caption/mmproj-SmolVLM-500M-Instruct-Q8_0.gguf"
  [motion]="motion/motion-library.json"
  [segveg]="segment/meshseg_vegetation.onnx"
  [segveh]="segment/meshseg_vehicle.onnx"
  [segbld]="segment/meshseg_building.onnx"
  [segcls]="segment/meshseg_category.onnx"
  [blazeface]="mocap/face/face_detector.onnx"
  [facemesh]="mocap/face/face_landmarks.onnx"
  [faceblendshapes]="mocap/face/face_blendshapes.onnx"
  [blazepose]="mocap/pose/pose_detector.onnx"
  [poselandmarks]="mocap/pose/pose_landmarks.onnx"
)

sync_one() {
  local model=$1 repo="$OWNER/${REPOS[$model]}"
  echo "=== $model → $repo"
  for f in ${FILES[$model]}; do
    echo "  $f"
    # `hf` replaces the removed `huggingface-cli` (huggingface_hub >= 1.0).
    hf download "$AGG" "$f" --local-dir "$WORK" --quiet
    hf upload "$repo" "$WORK/$f" "$(basename "$f")" --quiet
    rm -f "$WORK/$f"          # multi-GB files — don't accumulate
  done
}

if [ $# -gt 0 ]; then
  for m in "$@"; do sync_one "$m"; done
else
  for m in "${!FILES[@]}"; do sync_one "$m"; done
fi
echo "done"
