#!/usr/bin/env bash
# Upload the v5 flow-matching text-to-motion model to the QtMeshEditor HF
# models repo (#840/#858, epic #837). ONE-TIME, run by a maintainer with
# write access.
#
# The app downloads these on first use from
#   https://huggingface.co/fernandotonon/QtMeshEditor-models/resolve/main/motion/t2m.onnx
#   https://huggingface.co/fernandotonon/QtMeshEditor-models/resolve/main/motion/t2m-vocab.json
# The v5 ONNX keeps the exact v4 runtime interface (tokens[1,V], seed[1,Z]
# -> motion[1,T,220]) so replacing the files is backward-compatible: older
# builds run the new model too (they ignore the vocab's restWorld/restDir
# and fall back to the synthetic-standing-pose path). The previous v4 files
# are preserved under versioned names for rollback.
#
# Prereqs:
#   pip install -U "huggingface_hub[cli]"
#   huggingface-cli login          # token with write access
#   train-t2m-flow-v5.py already run -> OUT_DIR holds t2m.onnx + t2m-vocab.json
#
# Usage:
#   OUT_DIR=/tmp/t2m_v5_flow ./scripts/upload-t2m-v5-model.sh
set -euo pipefail

REPO="${REPO:-fernandotonon/QtMeshEditor-models}"
OUT_DIR="${OUT_DIR:?set OUT_DIR to the training output dir (t2m.onnx + t2m-vocab.json)}"

for f in t2m.onnx t2m-vocab.json; do
    [[ -f "$OUT_DIR/$f" ]] || { echo "missing $OUT_DIR/$f" >&2; exit 1; }
done

# keep the previous (v4 CVAE) files for rollback under versioned names.
# IDEMPOTENT: only back up if the v4 rollback does NOT already exist —
# otherwise a re-run would overwrite the real v4 with the current (v5) file
# and destroy the rollback point.
TMP=$(mktemp -d)
for f in t2m.onnx t2m-vocab.json; do
    case "$f" in
        t2m.onnx)       dst="motion/t2m-v4.onnx" ;;
        t2m-vocab.json) dst="motion/t2m-vocab-v4.json" ;;
    esac
    if huggingface-cli download "$REPO" "$dst" --local-dir "$TMP" >/dev/null 2>&1; then
        echo "rollback $dst already exists — skipping backup (idempotent)"
        continue
    fi
    if huggingface-cli download "$REPO" "motion/$f" --local-dir "$TMP" >/dev/null 2>&1; then
        huggingface-cli upload "$REPO" "$TMP/motion/$f" "$dst" \
            --commit-message "t2m: preserve v4 CVAE as $dst before the v5 flow model (#858)"
    fi
done

huggingface-cli upload "$REPO" "$OUT_DIR/t2m.onnx" motion/t2m.onnx \
    --commit-message "t2m v5: flow-matching model, canonical reference triple (#840/#858)"
huggingface-cli upload "$REPO" "$OUT_DIR/t2m-vocab.json" motion/t2m-vocab.json \
    --commit-message "t2m v5: vocab + canonical reference triple (#840/#858)"
echo "uploaded v5 t2m model + vocab to $REPO/motion/"
