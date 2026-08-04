#!/usr/bin/env bash
# Install runtime deps for ONNX Runtime CUDA EP (Linux, ORT 1.20.x + CUDA 12).
# Downloads cuDNN 9 + cuBLAS into .cache/cuda-deps/ (no sudo). QtMeshEditor picks
# this up automatically via OnnxRuntimeSettings::prepareRuntimeEnvironment().

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEST="$ROOT/.cache/cuda-deps"

if ! command -v nvidia-smi >/dev/null 2>&1; then
    echo "No NVIDIA GPU driver (nvidia-smi) found." >&2
    exit 1
fi

echo "GPU: $(nvidia-smi --query-gpu=name --format=csv,noheader | head -1)"
mkdir -p "$DEST"

if [[ -f "$DEST/nvidia/cudnn/lib/libcudnn.so.9" ]]; then
    echo "cuDNN already present at $DEST/nvidia/cudnn/lib"
else
    echo "Installing nvidia-cudnn-cu12 + nvidia-cublas-cu12 into $DEST ..."
    python3 -m pip install \
        nvidia-cudnn-cu12 nvidia-cublas-cu12 \
        -t "$DEST" --no-cache-dir --upgrade
fi

if [[ ! -f "$DEST/nvidia/cudnn/lib/libcudnn.so.9" ]]; then
    echo "cuDNN install failed." >&2
    exit 1
fi

if [[ ! -f "$DEST/nvidia/cublas/lib/libcublas.so.12" ]]; then
    echo "cuBLAS install failed (expected under $DEST/nvidia/cublas/lib)." >&2
    exit 1
fi

echo "Done. Rebuild with GPU ONNX Runtime if needed:"
echo "  cmake . -B build_local -DENABLE_ONNX=ON -DQTMESH_ONNX_GPU=ON"
echo "  cmake --build build_local --target QtMeshEditor -j4"
echo ""
echo "Enable AI Settings → Prefer GPU for ONNX models, then run UniRig."
echo "Debug: QTMESH_ONNX_DEBUG=1 ./build_local/bin/qtmesh rig model.obj --algo unirig -o out.fbx"
