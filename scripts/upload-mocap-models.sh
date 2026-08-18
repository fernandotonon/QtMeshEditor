#!/usr/bin/env bash
# Upload the performance-capture models to the QtMeshEditor HF models repo
# (epic #869, slice #876 — model hosting). ONE-TIME, run by a maintainer with
# write access.
#
# The app downloads these on first use from
#   https://huggingface.co/fernandotonon/QtMeshEditor-models/resolve/main/mocap/{face,pose}/...
# so the file names + subfolders below MUST match FaceCapPredictor /
# PoseCapPredictor's kDetectorFile/kLandmarksFile/kBlendshapesFile.
#
# All five graphs are converted Google MediaPipe models — Apache-2.0 code AND
# weights (see THIRD_PARTY_AI_MODELS.md). The Apache-2.0 LICENSE/NOTICE is
# uploaded next to the weights as the license requires.
#
# Prereqs:
#   pip install -U huggingface_hub
#   hf auth login                  # a token with write access to the repo
#   scripts/export-facecap-onnx.py already run → OUT_DIR holds face/ + pose/
#
# Usage:
#   OUT_DIR=.mocap_work/out ./scripts/upload-mocap-models.sh
set -euo pipefail

REPO="${REPO:-fernandotonon/QtMeshEditor-models}"
OUT_DIR="${OUT_DIR:?set OUT_DIR to the export output dir (face/*.onnx + pose/*.onnx)}"

upload() {  # <local> <repo-path>
    local src="$1" dst="$2"
    # Every mocap graph is REQUIRED — a partial upload produces a broken
    # model release (the app downloads all five). Fail hard rather than
    # skipping, so `set -e` aborts before a half-published set goes live.
    if [ ! -f "$src" ]; then
        echo "!! ABORT: required model missing: $src" >&2
        exit 1
    fi
    echo ">> uploading $src -> $REPO:$dst"
    # `hf upload <repo> <local> <path-in-repo>` (huggingface_hub >= 1.0;
    # the old `huggingface-cli upload` was removed).
    hf upload "$REPO" "$src" "$dst"
}

# Face bundle (FaceCapPredictor): detector + landmarks + blendshapes.
upload "$OUT_DIR/face/face_detector.onnx"    "mocap/face/face_detector.onnx"
upload "$OUT_DIR/face/face_landmarks.onnx"   "mocap/face/face_landmarks.onnx"
upload "$OUT_DIR/face/face_blendshapes.onnx" "mocap/face/face_blendshapes.onnx"

# Pose bundle (PoseCapPredictor): detector + landmarks.
upload "$OUT_DIR/pose/pose_detector.onnx"   "mocap/pose/pose_detector.onnx"
upload "$OUT_DIR/pose/pose_landmarks.onnx"  "mocap/pose/pose_landmarks.onnx"

# Hands (HandCapPredictor): MediaPipe Hands 21-landmark graph + BlazePalm detector.
# Unity's Apache-2.0 ONNX conversion is accepted as a source
# (hand_landmarks_detector.onnx → hand_landmarks.onnx).
if [ -f "$OUT_DIR/hands/hand_landmarks.onnx" ]; then
    upload "$OUT_DIR/hands/hand_landmarks.onnx" "mocap/hands/hand_landmarks.onnx"
fi
if [ -f "$OUT_DIR/hands/hand_detector.onnx" ]; then
    upload "$OUT_DIR/hands/hand_detector.onnx" "mocap/hands/hand_detector.onnx"
fi

# Apache-2.0 notice next to the weights (MediaPipe attribution).
NOTICE="$(mktemp)"
cat > "$NOTICE" <<'EOF'
These ONNX graphs are converted from Google MediaPipe's Face Landmarker and
Pose Landmarker models (https://developers.google.com/mediapipe), released
under the Apache License, Version 2.0:
  https://www.apache.org/licenses/LICENSE-2.0
Conversion: QtMeshEditor scripts/export-facecap-onnx.py (numerical parity
asserted against the Python mediapipe reference; see docs/MOCAP_SPIKE.md in
https://github.com/fernandotonon/QtMeshEditor).
EOF
upload "$NOTICE" "mocap/NOTICE.txt"
rm -f "$NOTICE"

echo "done. Verify: curl -sI https://huggingface.co/$REPO/resolve/main/mocap/face/face_detector.onnx | head -1"
