#!/usr/bin/env bash
# One-shot driver for the TripoSG ONNX export (see scripts/export-triposg-onnx.py).
# Reuses the TripoSR export venv from /tmp/rmib_train, adds the missing deps,
# clones the upstream repo, runs the export with ORT verification, and leaves
# the graphs in /tmp/triposg_export/dist/triposg_onnx ready for HF upload.
#
# Usage:  bash scripts/run-triposg-export.sh
# Runtime: ~8 GB HF download + CPU export — expect 1-2 hours.
set -euo pipefail

VENV=/tmp/rmib_train/triposr_venv
WORK=/tmp/triposg_export
REPO_SCRIPT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/export-triposg-onnx.py"

mkdir -p "$WORK"
cd "$WORK"

echo "[deps] installing diffusers/jaxtyping/accelerate into $VENV"
"$VENV/bin/pip" install -q "diffusers==0.30.3" jaxtyping accelerate

if [ ! -d TripoSG ]; then
  echo "[clone] VAST-AI-Research/TripoSG"
  git clone --depth 1 https://github.com/VAST-AI-Research/TripoSG
fi

echo "[export] running (log: $WORK/export.log)"
"$VENV/bin/python" "$REPO_SCRIPT" --triposg ./TripoSG --out dist/triposg_onnx --verify \
  2>&1 | tee "$WORK/export.log"

echo "[done] artifacts:"
ls -lh dist/triposg_onnx
echo
echo "Next: upload to fernandotonon/QtMeshEditor-models under triposg/"
echo "  (mirror scripts/upload-triposr-models.sh — includes the .onnx.data sidecar)"
