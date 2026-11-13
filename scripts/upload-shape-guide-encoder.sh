#!/usr/bin/env bash
# Upload the TRELLIS sparse-structure ENCODER (shape guide) to the QtMeshEditor
# HF models repo. ONE-TIME, run by a maintainer with write access.
#
# The app downloads it on first use of a shape guide from
#   https://huggingface.co/fernandotonon/QtMeshEditor-models/resolve/main/trellis2/ss_encoder.onnx
# and verifies its SHA-256 (kModelSha256 in src/ImageTo3D/ShapeGuide.cpp) —
# re-exporting changes nothing (the export is deterministic), but if it ever
# does, bump the digest in the same change.
#
# The weights are microsoft/TRELLIS-image-large's ss_enc_conv3d_16l8_fp16 (MIT).
#
# Usage:
#   ONNX=path/to/ss_encoder.onnx ./scripts/upload-shape-guide-encoder.sh
set -euo pipefail
REPO="${REPO:-fernandotonon/QtMeshEditor-models}"
ONNX="${ONNX:?set ONNX to the exported ss_encoder.onnx}"
want=$(grep -o '"[0-9a-f]\{64\}"' "$(dirname "$0")/../src/ImageTo3D/ShapeGuide.cpp" | tr -d '"')
have=$(shasum -a 256 "$ONNX" | cut -d' ' -f1)
if [ "$want" != "$have" ]; then
    echo "ABORT: $ONNX sha256 $have does not match the pinned $want"; exit 1
fi
hf upload "$REPO" "$ONNX" trellis2/ss_encoder.onnx
echo "uploaded trellis2/ss_encoder.onnx ($have)"
