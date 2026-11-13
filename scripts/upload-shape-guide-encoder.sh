#!/usr/bin/env bash
# Upload the TRELLIS sparse-structure ENCODER (shape guide) to the QtMeshEditor
# HF repos. ONE-TIME, run by a maintainer with write access.
#
# The app downloads it on first use of a shape guide from
#   https://huggingface.co/fernandotonon/QtMeshEditor-models/resolve/main/trellis2/ss_encoder.onnx
# and verifies its SHA-256 (kModelSha256 in src/ImageTo3D/ShapeGuide.cpp):
# a re-export must re-pin the digest in the same change. The file is also
# mirrored to the dedicated repo QtMeshEditor-trellis-ss-encoder-onnx, with its
# model card (scripts/trellis_ss_encoder_model_card.md) and the MIT licence
# (see sync-hf-model-repos.sh).
#
# The weights are microsoft/TRELLIS-image-large's ss_enc_conv3d_16l8_fp16 (MIT).
#
# Prereqs: pip install -U "huggingface_hub[cli]" && hf auth login
# Usage:
#   ONNX=path/to/ss_encoder.onnx ./scripts/upload-shape-guide-encoder.sh
#   DRY_RUN=1 ... checks the digest, licence and card without uploading.
# Big HF transfers drop connections routinely: each upload is retried.
set -euo pipefail
AGG="${AGG:-${REPO:-fernandotonon/QtMeshEditor-models}}"
DED="${DED:-fernandotonon/QtMeshEditor-trellis-ss-encoder-onnx}"
ONNX="${ONNX:?set ONNX to the exported ss_encoder.onnx}"
HFCLI="${HFCLI:-hf}"
HERE="$(cd "$(dirname "$0")" && pwd)"
LICENSE="$HERE/LICENSE-TRELLIS.txt"
CARD="$HERE/trellis_ss_encoder_model_card.md"

want=$(grep -o '"[0-9a-f]\{64\}"' "$HERE/../src/ImageTo3D/ShapeGuide.cpp" | tr -d '"')
have=$(shasum -a 256 "$ONNX" | cut -d' ' -f1)
if [ "$want" != "$have" ]; then
    echo "ABORT: $ONNX sha256 $have does not match the pinned $want"; exit 1
fi
[ -f "$LICENSE" ] || { echo "ABORT: missing $LICENSE"; exit 1; }
[ -f "$CARD" ] || { echo "ABORT: missing $CARD"; exit 1; }
if [ "${DRY_RUN:-0}" = "1" ]; then
    echo "dry run: digest $have matches; licence + card present. Nothing uploaded."
    exit 0
fi

retry() { local n=0; until "$@"; do n=$((n+1)); [ $n -ge 8 ] && return 1; echo "retry $n..."; sleep 10; done; }

"$HFCLI" repo create "$DED" --repo-type model --exist-ok >/dev/null

STAGE=$(mktemp -d)
trap 'rm -rf "$STAGE"' EXIT
mkdir -p "$STAGE/agg" "$STAGE/ded"
for d in agg ded; do
    ln -s "$(cd "$(dirname "$ONNX")" && pwd)/$(basename "$ONNX")" "$STAGE/$d/ss_encoder.onnx"
    cp "$LICENSE" "$STAGE/$d/LICENSE-TRELLIS.txt"
done
cp "$CARD" "$STAGE/ded/README.md"

echo ">> $AGG:trellis2/"
retry "$HFCLI" upload "$AGG" "$STAGE/agg" trellis2 \
    --commit-message "Add TRELLIS sparse-structure encoder (shape guide, MIT)"
echo ">> $DED/ (+ README)"
retry "$HFCLI" upload "$DED" "$STAGE/ded" . \
    --commit-message "Add TRELLIS sparse-structure encoder (shape guide, MIT)"

echo "uploaded ss_encoder.onnx ($have). Verify:"
echo "  curl -sIL https://huggingface.co/$AGG/resolve/main/trellis2/ss_encoder.onnx | grep -i -m1 '^HTTP'"
echo "  curl -sIL https://huggingface.co/$DED/resolve/main/ss_encoder.onnx | grep -i -m1 '^HTTP'"
