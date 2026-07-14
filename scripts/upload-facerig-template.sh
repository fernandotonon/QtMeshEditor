#!/usr/bin/env bash
# Upload the ARKit face-rig template to the QtMeshEditor HF models repo
# (epic #889, slice #890). ONE-TIME, run by a maintainer with write access.
#
# The app downloads this on first use from
#   https://huggingface.co/fernandotonon/QtMeshEditor-models/resolve/main/facerig/arkit_template.bin
# so the file name + subdir MUST match ArkitTemplate::modelPath() /
# kDefaultModelBaseUrl.
#
# The template is derived from ICT-FaceKit (USC-ICT), MIT — ship the ICT MIT
# LICENSE next to it (see THIRD_PARTY_AI_MODELS.md).
#
# Prereqs:
#   pip install -U huggingface_hub
#   hf auth login                  # a token with write access to the repo
#   scripts/export-arkit-template.py already run -> OUT holds arkit_template.bin
#
# Usage:
#   OUT=.facerig_work/out ICT_LICENSE=.facerig_work/ICT_LICENSE.txt \
#     ./scripts/upload-facerig-template.sh
set -euo pipefail

REPO="${REPO:-fernandotonon/QtMeshEditor-models}"
OUT="${OUT:?set OUT to the dir holding arkit_template.bin}"
ICT_LICENSE="${ICT_LICENSE:-}"

upload() {  # <local> <repo-path>
    local src="$1" dst="$2"
    if [ -f "$src" ]; then
        echo ">> uploading $src -> $REPO:$dst"
        hf upload "$REPO" "$src" "$dst"
    else
        echo "!! skip (missing): $src"
    fi
}

upload "$OUT/arkit_template.bin" "facerig/arkit_template.bin"
[ -n "$ICT_LICENSE" ] && upload "$ICT_LICENSE" "facerig/ICT-FaceKit-LICENSE.txt"

echo "done. Verify: curl -sI https://huggingface.co/$REPO/resolve/main/facerig/arkit_template.bin | head -1"
