#!/usr/bin/env bash
#
# bake_and_stage.sh (Unity edition)
# --------------------------------
# Bakes a VAT for `--target unity` and stages everything into
# tools/unity-vat-test/Assets/VAT/Bakes/<basename_anim>/.
# Unity auto-imports the .png + .meta sidecar on next focus.
#
# Run from the QtMeshEditor repo root:
#   ./tools/unity-vat-test/bake_and_stage.sh \
#       "media/models/Rumba Dancing.fbx" "mixamo.com"

set -euo pipefail

if [[ $# -lt 2 ]]; then
    cat <<EOF
Usage: $0 <mesh-file> <animation-name> [--fps N]

  Bakes OpenVAT (single packed 16-bit RGB PNG + os-remap JSON).
  Stages into Assets/VAT/Bakes/<basename>_<anim>/.

Example:
  $0 "media/models/Rumba Dancing.fbx" "mixamo.com"
EOF
    exit 2
fi

MESH="$1"; shift
ANIM="$1"; shift
FPS=30
while [[ $# -gt 0 ]]; do
    case "$1" in
        --fps) FPS="$2"; shift 2 ;;
        *) echo "Unknown arg: $1"; exit 2 ;;
    esac
done

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" &> /dev/null && pwd)"
REPO_ROOT="$(cd -- "$SCRIPT_DIR/../.." && pwd)"
LOCAL_BIN="$REPO_ROOT/build_local/bin/QtMeshEditor.app/Contents/MacOS/QtMeshEditor"
if [[ -x "$LOCAL_BIN" ]]; then
    QTM="$LOCAL_BIN"
elif command -v qtmesh &> /dev/null; then
    QTM=qtmesh
else
    echo "Error: qtmesh not found. Build with 'cmake --build build_local --target QtMeshEditor' or install qtmesh." >&2
    exit 1
fi

if [[ ! -f "$MESH" ]]; then
    echo "Error: mesh file not found: $MESH" >&2
    exit 1
fi

BASENAME=$(basename "$MESH" | sed 's/\.[^.]*$//')
SAFE_ANIM=$(echo "$ANIM" | tr -c '[:alnum:]._-' '_')
STAGE_DIR="$SCRIPT_DIR/Assets/VAT/Bakes/${BASENAME}_${SAFE_ANIM}"

echo ">> Staging into $STAGE_DIR"
rm -rf "$STAGE_DIR"
mkdir -p "$STAGE_DIR"

echo ">> Baking OpenVAT (${FPS}fps)"
"$QTM" vat "$MESH" --anim "$ANIM" --fps "$FPS" -o "$STAGE_DIR"

echo ">> Converting source mesh to glTF (preserves vertex order Unity will see)"
"$QTM" convert "$MESH" -o "$STAGE_DIR/source.gltf"


echo
echo "Done. In Unity:"
echo "  1. Open this project: $SCRIPT_DIR/"
echo "     Unity Hub → Add → select tools/unity-vat-test/"
echo "  2. Open scene: Assets/VAT/Scenes/VATTest.unity"
echo "  3. The VATPlayer auto-fills from sidecar.json + bake textures."
echo
echo "Files staged:"
ls -la "$STAGE_DIR"
