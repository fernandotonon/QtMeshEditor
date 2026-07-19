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

# Required artifacts — refuse to publish an incomplete release. Only the
# landmark model below is optional (hosted separately by the mocap epic).
[ -f "$OUT/arkit_template.bin" ] || { echo "ABORT: missing $OUT/arkit_template.bin"; exit 1; }
[ -n "$ICT_LICENSE" ] && [ -f "$ICT_LICENSE" ] || { echo "ABORT: ICT_LICENSE not set or missing (the MIT license must ship next to the template)"; exit 1; }
upload "$OUT/arkit_template.bin" "facerig/arkit_template.bin"
upload "$ICT_LICENSE" "facerig/ICT-FaceKit-LICENSE.txt"

# Facial-landmark model (MediaPipe FaceMesh V2, Apache-2.0) that anchors the
# NRICP fit to real face features (#889 landmark pass). It is the SAME graph the
# mocap face-capture uses (mocap/face/face_landmarks.onnx); we host a copy under
# facerig/ so the face-rig feature is self-contained. Point FACE_LMK at the
# converted onnx (e.g. .mocap_work/out/face/face_landmarks.onnx).
FACE_LMK="${FACE_LMK:-}"
[ -n "$FACE_LMK" ] && upload "$FACE_LMK" "facerig/face_landmarks.onnx"

echo "done. Verify: curl -sI https://huggingface.co/$REPO/resolve/main/facerig/arkit_template.bin | head -1"
