#!/usr/bin/env bash
# Create the five dedicated per-graph Hugging Face mirror repos for the
# performance-capture models (epic #869) and push each .onnx + its own model
# card. ONE-TIME, run by a maintainer with HF write access.
#
# The aggregate repo fernandotonon/QtMeshEditor-models (mocap/{face,pose}/) is
# the SOURCE OF TRUTH the app downloads from; these mirrors carry standalone
# model cards for discoverability (the per-model repo convention documented in
# THIRD_PARTY_AI_MODELS.md and scripts/sync-hf-model-repos.sh). After a
# re-export + re-upload to the aggregate, refresh weights with
# scripts/sync-hf-model-repos.sh (the mocap-* mirrors were added there).
#
# All five graphs are converted Google MediaPipe models — Apache-2.0 code AND
# weights.
#
# Prereqs:
#   pip install -U huggingface_hub
#   hf auth login                  # a token with write access under the owner
#   OUT_DIR holds face/*.onnx + pose/*.onnx (scripts/export-facecap-onnx.py)
#
# Usage:
#   OUT_DIR=.mocap_work/out ./scripts/create-mocap-mirror-repos.sh
set -euo pipefail

OWNER="${OWNER:-fernandotonon}"
OUT_DIR="${OUT_DIR:?set OUT_DIR to the export output dir (face/*.onnx + pose/*.onnx)}"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

# model-key → "repo-name|local-onnx|upstream-source|one-line-summary"
make_repo() {
    local repo="$1" src="$2" card="$3"
    echo "=== $OWNER/$repo"
    hf repo create "$OWNER/$repo" --repo-type model -y >/dev/null 2>&1 || true
    printf '%s\n' "$card" > "$WORK/README.md"
    hf upload "$OWNER/$repo" "$WORK/README.md" README.md >/dev/null
    hf upload "$OWNER/$repo" "$src" "$(basename "$src")" >/dev/null
    echo "    -> https://huggingface.co/$OWNER/$repo"
}

COMMON_FOOTER='
## License

Apache-2.0 — this graph is a direct ONNX conversion of a Google
[MediaPipe](https://developers.google.com/mediapipe) model (Apache-2.0 code
AND weights). Conversion + numerical-parity proof (vs the Python `mediapipe`
reference): [`scripts/export-facecap-onnx.py`](https://github.com/fernandotonon/QtMeshEditor/blob/master/scripts/export-facecap-onnx.py),
contract in [`docs/MOCAP_SPIKE.md`](https://github.com/fernandotonon/QtMeshEditor/blob/master/docs/MOCAP_SPIKE.md).

## How it is used

Mirror of one graph from [`fernandotonon/QtMeshEditor-models`](https://huggingface.co/fernandotonon/QtMeshEditor-models)
(`mocap/…`), which [QtMeshEditor](https://github.com/fernandotonon/QtMeshEditor)
downloads on first use for its **Performance Capture** feature (video/webcam →
facial morph + head + full-body skeletal animation, epic #869). This standalone
repo is for discoverability; the app fetches from the aggregate repo.'

make_repo "QtMeshEditor-blazeface-onnx" "$OUT_DIR/face/face_detector.onnx" \
"---
license: apache-2.0
tags: [onnx, mediapipe, face-detection, blazeface, qtmesheditor]
---

# BlazeFace short-range face detector (ONNX)

Google MediaPipe's BlazeFace short-range detector, from the Face Landmarker
\`.task\` bundle, converted to ONNX.

- **Input** \`input\` \`[1,128,128,3]\` RGB, normalized to \`[-1,1]\`, keep-aspect
  letterboxed (zero border).
- **Outputs** \`regressors\` \`[1,896,16]\` (box + 6 keypoints per anchor),
  \`classificators\` \`[1,896,1]\` (score logits). SSD anchors: strides
  \`[8,16,16,16]\`, min/max scale 0.1484375/0.75 → 896 anchors; decode + weighted
  NMS at IoU 0.3.
- Feeds the face ROI (rotation from the eye keypoints, 1.5× square-long) to the
  Face Mesh landmark model.
$COMMON_FOOTER"

make_repo "QtMeshEditor-facemesh-onnx" "$OUT_DIR/face/face_landmarks.onnx" \
"---
license: apache-2.0
tags: [onnx, mediapipe, face-landmarks, face-mesh, qtmesheditor]
---

# Face Mesh V2 landmark model (ONNX)

Google MediaPipe's Face Landmarks Detector (Face Mesh V2), converted to ONNX.

- **Input** \`[N,256,256,3]\` RGB in \`[0,1]\` — the rotated, cropped face ROI.
- **Outputs** \`[N,1,1,1434]\` = **478 landmarks × (x,y,z)** in 256-crop pixels,
  plus a face-presence logit. Landmarks project back through the inverse ROI
  transform; a 146-landmark subset feeds the blendshape model.
$COMMON_FOOTER"

make_repo "QtMeshEditor-faceblendshapes-onnx" "$OUT_DIR/face/face_blendshapes.onnx" \
"---
license: apache-2.0
tags: [onnx, mediapipe, blendshapes, arkit, face-capture, qtmesheditor]
---

# Face blendshapes MLP-Mixer (ONNX)

Google MediaPipe's face blendshape model (MLP-Mixer), converted to ONNX.

- **Input** \`[1,146,2]\` — the 146-landmark subset as **pixel coords**
  (\`x·imgW, y·imgH\`).
- **Output** \`[52]\` — **52 ARKit-compatible blendshape scores** in \`[0,1]\`,
  in MediaPipe order (\`_neutral\`, \`browDownLeft\`, … \`jawOpen\`, …
  \`mouthSmileLeft/Right\`, … \`eyeBlinkLeft/Right\`, …). These drive ARKit-style
  morph targets directly.

Converted with tf2onnx optimizers disabled (the optimized graph broke a
LayerNorm under ONNX Runtime — see the spike doc).
$COMMON_FOOTER"

make_repo "QtMeshEditor-blazepose-onnx" "$OUT_DIR/pose/pose_detector.onnx" \
"---
license: apache-2.0
tags: [onnx, mediapipe, pose-detection, blazepose, qtmesheditor]
---

# BlazePose person detector (ONNX)

Google MediaPipe's BlazePose detector, from the Pose Landmarker \`.task\`
bundle, converted to ONNX.

- **Input** \`[1,224,224,3]\` RGB in \`[-1,1]\`, letterboxed.
- **Outputs** \`[1,2254,12]\` + \`[1,2254,1]\`. Anchors: strides
  \`[8,16,32,32,32]\` → 2254. The ROI is centred on the mid-hip keypoint,
  sized from the hip→scale keypoint distance, target angle 90°, 1.25× square.

The detector ships fp16 **block-sparse** weights behind DENSIFY ops that
tf2onnx can't parse; the export densifies them via the TFLite interpreter
first (see the export script).
$COMMON_FOOTER"

make_repo "QtMeshEditor-poselandmarks-onnx" "$OUT_DIR/pose/pose_landmarks.onnx" \
"---
license: apache-2.0
tags: [onnx, mediapipe, pose-landmarks, blazepose, body-capture, qtmesheditor]
---

# BlazePose full landmark model (ONNX)

Google MediaPipe's Pose Landmarks Detector (BlazePose full), converted to
ONNX.

- **Input** \`[1,256,256,3]\` RGB in \`[0,1]\` — the rotated, cropped person ROI.
- **Outputs** \`[1,195]\` = 39 × (x,y,z,visibility,presence) screen landmarks
  in 256-crop pixels; \`[1,1]\` pose-presence probability; \`[1,256,256,1]\`
  segmentation; \`[1,64,64,39]\` heatmap; **\`[1,117]\` = 39 × (x,y,z) WORLD
  landmarks in metres, hip-centred** (the input to the analytic body-pose /
  IK solver — landmarks 0–32 are the 33 real body joints).
$COMMON_FOOTER"

echo "done. Refresh weights later via scripts/sync-hf-model-repos.sh"
