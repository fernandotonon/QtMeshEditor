#!/usr/bin/env bash
# Upload the SAM 2.1 Hiera-Small ONNX graphs (generic segmentation, #1179) to the QtMeshEditor HF repos.
# ONE-TIME, run by a maintainer with write access, AFTER `scripts/export-sam2-onnx.py` produced OUT_DIR.
#
# The app downloads from the aggregate repo (src/GenericSeg/SamMaskProvider.cpp kDefaultBaseUrl):
#   https://huggingface.co/fernandotonon/QtMeshEditor-models/resolve/<commit>/segment/<file>
# and every file is mirrored to the dedicated repo QtMeshEditor-sam2-onnx (see sync-hf-model-repos.sh),
# whose card is scripts/sam2_model_card.md. Meta's Apache-2.0 text travels with the graphs.
#
# NB: the app's default URL is pinned to an HF COMMIT together with the SHA-256 table in
# SamMaskProvider.cpp. After uploading NEW files, update the revision and the digests there.
#
# Prereqs: pip install -U "huggingface_hub[cli]" ; hf auth login (write token)
# Usage:   OUT_DIR=~/sam2_export/out SAM2_LICENSE=~/sam2_export/sam2/LICENSE ./scripts/upload-sam2-models.sh
set -euo pipefail

AGG="${AGG:-fernandotonon/QtMeshEditor-models}"
DED="${DED:-fernandotonon/QtMeshEditor-sam2-onnx}"
OUT_DIR="${OUT_DIR:?set OUT_DIR to the export output dir}"
SAM2_LICENSE="${SAM2_LICENSE:?set SAM2_LICENSE to the LICENSE file of the facebookresearch/sam2 checkout}"
HERE="$(cd "$(dirname "$0")" && pwd)"

grep -q "Apache License" "$SAM2_LICENSE" || { echo "$SAM2_LICENSE is not the Apache-2.0 text" >&2; exit 1; }
cp "$SAM2_LICENSE" "$OUT_DIR/LICENSE-SAM2.txt"
FILES=(LICENSE-SAM2.txt sam2.1_hiera_small_decoder.onnx sam2.1_hiera_small_encoder.onnx)

retry() { local n=0; until "$@"; do n=$((n+1)); [ $n -ge 8 ] && return 1; echo "retry $n..."; sleep 10; done; }

if hf repos --help >/dev/null 2>&1; then hf repos create "$DED" --repo-type model --exist-ok
else hf repo create "$DED" --repo-type model --exist-ok; fi
for f in "${FILES[@]}"; do
    [ -f "$OUT_DIR/$f" ] || { echo "missing $OUT_DIR/$f" >&2; exit 1; }
    echo ">> $f -> $AGG:segment/$f"
    retry hf upload "$AGG" "$OUT_DIR/$f" "segment/$f" --commit-message "SAM 2.1 small ONNX: $f (#1179)"
done
for f in "${FILES[@]}"; do
    echo ">> $f -> $DED:$f"
    retry hf upload "$DED" "$OUT_DIR/$f" "$f" --commit-message "SAM 2.1 small ONNX: $f"
done
retry hf upload "$DED" "$HERE/sam2_model_card.md" README.md --commit-message "Model card"
echo "done. Pin this revision in SamMaskProvider.cpp:"
echo "  curl -s https://huggingface.co/api/models/$AGG | python3 -c 'import json,sys; print(json.load(sys.stdin)[\"sha\"])'"
