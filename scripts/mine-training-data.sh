#!/usr/bin/env bash
# Mine segmentation training data (#410) from a directory of RIGGED meshes.
#
# For every rigged mesh under <input-dir>, runs
#   qtmesh segment <mesh> --dump-training-data <out>/<name>.json
# which reads EXACT per-vertex part labels from the mesh's rig (bone weights ->
# bone name -> part). Each rigged asset becomes one free, perfectly-labelled
# training sample. Feed the resulting <out>/ dir to:
#   python scripts/export-meshseg-onnx.py --samples 6000 --real-data <out>/ ...
#
# This is an OFFLINE dev tool. It is NOT shipped and the app never runs it.
#
# LICENSING: only mine meshes you can legally train a REDISTRIBUTABLE model on
# (CC0 / CC-BY with attribution). The shipped weights are a derived work of the
# training data, so the project's permissive-redistribution bar applies (the
# same reason ShapeNet-Part / LAFAN1 / Mixamo are NOT used — see
# THIRD_PARTY_AI_MODELS.md). Quaternius (CC0), Poly Pizza (CC0) and Kenney (CC0)
# rigged characters are good sources.
#
# Usage:
#   scripts/mine-training-data.sh <input-dir> [output-dir]
#     input-dir   directory of rigged *.fbx / *.glb / *.gltf / *.dae meshes
#     output-dir  where to write *.json samples (default: ./mined_training_data)
set -euo pipefail

IN="${1:-}"
OUT="${2:-./mined_training_data}"
if [[ -z "$IN" || ! -d "$IN" ]]; then
  echo "Usage: $0 <input-dir-of-rigged-meshes> [output-dir]" >&2
  exit 2
fi

# Locate the qtmesh binary (build symlink, PATH, or common build dir).
QTMESH=""
for cand in ./build_local/bin/qtmesh ./qtmesh "$(command -v qtmesh 2>/dev/null || true)"; do
  if [[ -n "$cand" && -x "$cand" ]]; then QTMESH="$cand"; break; fi
done
if [[ -z "$QTMESH" ]]; then
  echo "Error: could not find the 'qtmesh' binary (build it, or add it to PATH)." >&2
  exit 1
fi
echo "Using qtmesh: $QTMESH"

mkdir -p "$OUT"
# Stay offline — mining only uses the rig, never the ONNX model.
export QTMESH_SEGMENT_NO_DOWNLOAD=1

total=0 ok=0 skip=0
while IFS= read -r -d '' mesh; do
  total=$((total+1))
  name="$(basename "$mesh")"
  name="${name%.*}"
  dest="$OUT/${name}.json"
  if "$QTMESH" segment "$mesh" --dump-training-data "$dest" >/dev/null 2>&1; then
    ok=$((ok+1))
    echo "  mined  $name"
  else
    skip=$((skip+1))
    # Most common reason: the mesh is static (no skeleton) or the rig is sparse.
    echo "  skip   $name (not skinned / sparse rig / unreadable)"
  fi
done < <(find "$IN" \( -iname '*.fbx' -o -iname '*.glb' -o -iname '*.gltf' -o -iname '*.dae' \) -print0)

echo
echo "Mined $ok / $total mesh(es) -> $OUT  (skipped $skip)"
echo "Next: python scripts/export-meshseg-onnx.py --samples 6000 --real-data \"$OUT\" --out meshseg.onnx"
