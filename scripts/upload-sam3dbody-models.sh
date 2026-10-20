#!/usr/bin/env bash
# Upload the SAM 3D Body + MHR ONNX graphs to the QtMeshEditor HF repos (#1100). ONE-TIME, run by a
# maintainer with write access, AFTER `scripts/export-sam3dbody-onnx.py` produced OUT_DIR.
#
# The app downloads from the aggregate repo (src/BodyFromPhoto/SamBodyPredictor.cpp kDefaultBaseUrl):
#   https://huggingface.co/fernandotonon/QtMeshEditor-models/resolve/main/sam3dbody/<file>
# and every file is mirrored to the dedicated repo QtMeshEditor-sam3dbody-onnx (see sync-hf-model-repos.sh).
# The SAM License requires a copy of itself to accompany the SAM materials, so both licence texts are uploaded
# next to the graphs and the dedicated repo's card (scripts/sam3dbody_model_card.md) states the terms.
#
# Prereqs: pip install -U "huggingface_hub[cli]" ; hf auth login (write token)
# Usage:   OUT_DIR=/path/to/export_out ./scripts/upload-sam3dbody-models.sh
# Big HF transfers drop connections routinely: each upload is retried (the CLI resumes).
set -euo pipefail

AGG="${AGG:-fernandotonon/QtMeshEditor-models}"
DED="${DED:-fernandotonon/QtMeshEditor-sam3dbody-onnx}"
OUT_DIR="${OUT_DIR:?set OUT_DIR to the export output dir}"
HERE="$(cd "$(dirname "$0")" && pwd)"
FILES=(LICENSE-SAM.txt LICENSE-MHR.txt mhr_faces.i32 mhr_uv.bin mhr_body.onnx sam3d_decoder.onnx sam3d_backbone.onnx sam3d_backbone.onnx.data)

retry() { local n=0; until "$@"; do n=$((n+1)); [ $n -ge 8 ] && return 1; echo "retry $n..."; sleep 10; done; }

# `hf repo create --exist-ok` succeeds when the repo exists and FAILS loudly on any real problem (auth, name)
hf repo create "$DED" --repo-type model --exist-ok
for f in "${FILES[@]}"; do
    [ -f "$OUT_DIR/$f" ] || { echo "missing $OUT_DIR/$f" >&2; exit 1; }
    echo ">> $f -> $AGG:sam3dbody/$f"
    retry hf upload "$AGG" "$OUT_DIR/$f" "sam3dbody/$f"
done
for f in "${FILES[@]}"; do
    echo ">> $f -> $DED:$f"
    retry hf upload "$DED" "$OUT_DIR/$f" "$f"
done
retry hf upload "$DED" "$HERE/sam3dbody_model_card.md" README.md
echo "done. Verify: curl -sIL https://huggingface.co/$AGG/resolve/main/sam3dbody/mhr_body.onnx | head -1"
