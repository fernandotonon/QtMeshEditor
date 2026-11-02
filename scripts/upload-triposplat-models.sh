#!/usr/bin/env bash
# Upload the TripoSplat ONNX graphs (+ licence texts) to the QtMeshEditor HF
# models repo. ONE-TIME, run by a maintainer with write access.
#
# The app downloads these on first use from
#   https://huggingface.co/fernandotonon/QtMeshEditor-models/resolve/main/triposplat/...
# so the names MUST match TripoSplatPredictor::modelFiles() + kLicenseFiles,
# and the files MUST be the ones whose SHA-256 is pinned in
# TripoSplatPredictor.cpp (kDigests) — a re-export changes them; re-pin.
#
# Every graph is a small proto + an .onnx.data external-weights file: BOTH
# must be hosted side by side or ONNX Runtime cannot load the graph.
#
# Licences that travel with the weights:
#   LICENSE-TripoSplat.txt — MIT (VAST-AI TripoSplat code + weights)
#   LICENSE-DINOv3.txt     — Meta DINOv3 License; sec. 1.b.i requires a copy
#                            with any redistribution of DINOv3 weights or
#                            derivatives (triposplat_dinov3.onnx is one).
#   (the FLUX.2 VAE encoder inside triposplat_vae.onnx is Apache-2.0)
#
# Prereqs:
#   pip install -U "huggingface_hub[cli]" && hf auth login
#   scripts/export-triposplat-onnx.py already run -> OUT_DIR holds the graphs
#   LIC_DIR holds LICENSE-TripoSplat.txt (upstream repo LICENSE) and
#   LICENSE-DINOv3.txt (github.com/facebookresearch/dinov3 LICENSE.md)
#
# Usage:
#   OUT_DIR=~/triposplat_eval/onnx LIC_DIR=~/triposplat_eval/licenses \
#       ./scripts/upload-triposplat-models.sh
#   DRY_RUN=1 ... checks digests + licences without uploading.
set -euo pipefail

REPO="${REPO:-fernandotonon/QtMeshEditor-models}"
OUT_DIR="${OUT_DIR:?set OUT_DIR to the exported graphs}"
LIC_DIR="${LIC_DIR:?set LIC_DIR to the licence texts}"
HFCLI="${HFCLI:-hf}"

# Refuse to upload files that do not match the digests the app pins.
expected() {
    grep -o "{\"$1\", *\"[0-9a-f]*\"}" "$(dirname "$0")/../src/ImageTo3D/TripoSplatPredictor.cpp" \
        | sed -E 's/.*"([0-9a-f]{64})".*/\1/'
}

for g in dinov3 vae flow octree gs; do
    for ext in onnx onnx.data; do
        f="triposplat_${g}.${ext}"
        [ -f "$OUT_DIR/$f" ] || { echo "!! missing $OUT_DIR/$f"; exit 1; }
        got=$(shasum -a 256 "$OUT_DIR/$f" | cut -d' ' -f1)
        want=$(expected "$f")
        if [ "$got" != "$want" ]; then
            echo "!! $f digest $got does not match the pinned $want — re-pin kDigests first"
            exit 1
        fi
    done
done
for l in LICENSE-TripoSplat.txt LICENSE-DINOv3.txt; do
    [ -f "$LIC_DIR/$l" ] || { echo "!! missing $LIC_DIR/$l"; exit 1; }
done

if [ "${DRY_RUN:-0}" = "1" ]; then
    echo "dry run: all 10 graph files match the pinned digests; licences present. Nothing uploaded."
    exit 0
fi

STAGE=$(mktemp -d)
trap 'rm -rf "$STAGE"' EXIT
mkdir -p "$STAGE/triposplat"
for g in dinov3 vae flow octree gs; do
    ln -s "$OUT_DIR/triposplat_${g}.onnx" "$STAGE/triposplat/"
    ln -s "$OUT_DIR/triposplat_${g}.onnx.data" "$STAGE/triposplat/"
done
cp "$LIC_DIR/LICENSE-TripoSplat.txt" "$LIC_DIR/LICENSE-DINOv3.txt" "$STAGE/triposplat/"

"$HFCLI" upload "$REPO" "$STAGE/triposplat" triposplat \
    --commit-message "Add TripoSplat ONNX graphs (MIT; DINOv3 License for the image encoder)"

echo "done. Verify:"
echo "  curl -sIL https://huggingface.co/$REPO/resolve/main/triposplat/triposplat_octree.onnx | grep -i -m1 '^HTTP'"
