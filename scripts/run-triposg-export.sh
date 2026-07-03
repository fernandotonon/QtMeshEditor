#!/usr/bin/env bash
# One-shot driver for the TripoSG ONNX export (see scripts/export-triposg-onnx.py).
# Creates a DEDICATED venv (do NOT reuse the TripoSR export venv — its
# transformers 4.35 pin conflicts with TripoSG's >=4.45 requirement, and a
# shared install drags huggingface_hub past <1.0 and breaks both), clones the
# upstream repo, runs the export with ORT verification, and leaves the graphs
# in /tmp/triposg_export/dist/triposg_onnx ready for HF upload.
#
# Usage:  bash scripts/run-triposg-export.sh
# Runtime: ~8 GB HF download + CPU export — expect 1-2 hours.
set -euo pipefail

WORK=/tmp/triposg_export
VENV="$WORK/venv"
REPO_SCRIPT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/export-triposg-onnx.py"

mkdir -p "$WORK"
cd "$WORK"

if [ ! -x "$VENV/bin/python" ]; then
  echo "[venv] creating $VENV"
  python3 -m venv "$VENV"
fi

echo "[deps] installing pinned export stack"
"$VENV/bin/pip" install -q --upgrade pip
"$VENV/bin/pip" install -q torch --index-url https://download.pytorch.org/whl/cpu
# huggingface_hub pinned <1.0: transformers 4.4x and diffusers 0.30.x both
# require it; unpinned installs pull 1.x and break transformers' version gate.
# The tail (trimesh…tqdm) are triposg-package IMPORT-time deps; the CUDA-only
# `diso` is deliberately absent — the export script stubs it.
"$VENV/bin/pip" install -q \
    "diffusers==0.30.3" \
    "transformers==4.46.3" \
    "huggingface_hub==0.26.5" \
    "tokenizers>=0.20,<0.21" \
    onnx onnxruntime numpy einops jaxtyping safetensors accelerate \
    trimesh scipy scikit-image omegaconf typeguard tqdm pillow

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
