#!/usr/bin/env bash
# Upload the SmolVLM image-captioning GGUFs to the QtMeshEditor HF models repo
# (image-to-3D "describe-then-generate" texture prompt, #764). ONE-TIME, run by
# a maintainer with write access.
#
# The app downloads these on first use from
#   https://huggingface.co/fernandotonon/QtMeshEditor-models/resolve/main/caption/...
# so the file names below MUST match ImageCaptioner's kModelFile / kMmprojFile.
#
# Source: ggml-org/SmolVLM-500M-Instruct-GGUF (Apache-2.0). Mirror both files
# (the model GGUF + its mmproj vision projector) into caption/ on our repo.
#
# Prereqs:
#   huggingface-cli login          # token with write access to the models repo
#   the two files downloaded to $SRC_DIR (see the download step below)
#
# Usage:
#   SRC_DIR=/tmp/smolvlm_dl ./scripts/upload-caption-models.sh
set -euo pipefail

REPO="${REPO:-fernandotonon/QtMeshEditor-models}"
SRC_DIR="${SRC_DIR:-/tmp/smolvlm_dl}"
HFCLI="${HFCLI:-huggingface-cli}"
command -v "$HFCLI" >/dev/null 2>&1 || HFCLI=/tmp/triposg_export/venv/bin/huggingface-cli

# One-time source fetch (idempotent — skips files already present):
#   HF_HUB_DISABLE_XET=1 "$HFCLI" download ggml-org/SmolVLM-500M-Instruct-GGUF \
#     SmolVLM-500M-Instruct-Q8_0.gguf mmproj-SmolVLM-500M-Instruct-Q8_0.gguf \
#     --local-dir "$SRC_DIR"

upload() {  # <file>
    local f="$1"
    if [ -f "$SRC_DIR/$f" ]; then
        echo ">> uploading $f -> $REPO:caption/$f"
        "$HFCLI" upload "$REPO" "$SRC_DIR/$f" "caption/$f"
    else
        echo "!! skip (missing): $SRC_DIR/$f"
    fi
}

upload "SmolVLM-500M-Instruct-Q8_0.gguf"
upload "mmproj-SmolVLM-500M-Instruct-Q8_0.gguf"

echo "done. Verify: curl -sI https://huggingface.co/$REPO/resolve/main/caption/mmproj-SmolVLM-500M-Instruct-Q8_0.gguf | head -1"
