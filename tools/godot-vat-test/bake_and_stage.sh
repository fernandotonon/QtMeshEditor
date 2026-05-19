#!/usr/bin/env bash
#
# bake_and_stage.sh
# -----------------
# One-shot helper that:
#   1. Bakes a VAT for `--target godot` from a given mesh + animation.
#   2. Converts the same mesh to glTF (Godot's preferred format).
#   3. Copies everything into tools/godot-vat-test/assets/<basename>/.
#   4. Patches scenes/Main.tscn so the VAT node's `bake_dir` points at it.
#
# Run from the QtMeshEditor repo root, e.g.:
#   ./tools/godot-vat-test/bake_and_stage.sh \
#       "media/models/Rumba Dancing.fbx" "mixamo.com"
#
# Then `godot --path tools/godot-vat-test scenes/Main.tscn`.

set -euo pipefail

if [[ $# -lt 2 ]]; then
    cat <<EOF
Usage: $0 <mesh-file> <animation-name> [--fps N]

  mesh-file        Path to the source mesh (.fbx, .gltf, .dae, .mesh, etc.)
  animation-name   Animation clip inside the mesh (use 'qtmesh anim <file> --list' to enumerate)

Output is always OpenVAT (single packed 16-bit RGB PNG + os-remap JSON).

Example:
  $0 "media/models/Rumba Dancing.fbx" "mixamo.com"
  $0 "media/models/Hip Hop Dancing.fbx" "mixamo.com" --fps 24
EOF
    exit 2
fi

MESH="$1"; shift
ANIM="$1"; shift

FPS=30

while [[ $# -gt 0 ]]; do
    case "$1" in
        --fps)
            if [[ $# -lt 2 || -z "${2:-}" ]]; then
                echo "Error: --fps requires a value" >&2
                exit 2
            fi
            FPS="$2"; shift 2 ;;
        *) echo "Unknown arg: $1"; exit 2 ;;
    esac
done

# Locate qtmesh — prefer the local build (has latest features) over a
# system-wide install (might be stale).
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

# Sanitize the animation name into a filesystem-safe base name. Mixamo
# clips often include spaces and dots ("mixamo.com").
BASENAME=$(basename "$MESH" | sed 's/\.[^.]*$//')
SAFE_ANIM=$(echo "$ANIM" | tr -c '[:alnum:]._-' '_')
STAGE_DIR="$SCRIPT_DIR/assets/${BASENAME}_${SAFE_ANIM}"

echo ">> Staging into $STAGE_DIR"
rm -rf "$STAGE_DIR"
mkdir -p "$STAGE_DIR"

echo ">> Baking OpenVAT (${FPS}fps)"
"$QTM" vat "$MESH" --anim "$ANIM" --fps "$FPS" -o "$STAGE_DIR"

echo ">> Converting source mesh to glTF (preserves vertex order Godot will see)"
"$QTM" convert "$MESH" -o "$STAGE_DIR/source.gltf"

# Patch the .tscn so the VAT node's `bake_dir` points at the new stage.
# Godot resolves res:// relative to project root.
TSCN="$SCRIPT_DIR/scenes/Main.tscn"
REL_PATH="res://assets/${BASENAME}_${SAFE_ANIM}"
GLTF_PATH="$REL_PATH/source.gltf"
echo ">> Patching $TSCN:"
echo "     bake_dir  = \"$REL_PATH\""
echo "     gltf_path = \"$GLTF_PATH\""
# macOS sed needs the explicit -i '' suffix; on Linux it's -i alone.
# Patch both the SkeletalLoader's gltf_path AND the VATPlayer's
# source_gltf so the VAT side runs against the same vertex order the
# baker observed.
if [[ "$OSTYPE" == "darwin"* ]]; then
    sed -i '' "s|^bake_dir = \".*\"|bake_dir = \"$REL_PATH\"|" "$TSCN"
    sed -i '' "s|^gltf_path = \".*\"|gltf_path = \"$GLTF_PATH\"|" "$TSCN"
    sed -i '' "s|^source_gltf = \".*\"|source_gltf = \"$GLTF_PATH\"|" "$TSCN"
else
    sed -i "s|^bake_dir = \".*\"|bake_dir = \"$REL_PATH\"|" "$TSCN"
    sed -i "s|^gltf_path = \".*\"|gltf_path = \"$GLTF_PATH\"|" "$TSCN"
    sed -i "s|^source_gltf = \".*\"|source_gltf = \"$GLTF_PATH\"|" "$TSCN"
fi

echo
echo "Done. Open the project in Godot 4:"
echo "  godot --path \"$SCRIPT_DIR\" scenes/Main.tscn"
echo
echo "Or from the editor: open $SCRIPT_DIR/project.godot and press F5."
echo
echo "Files staged:"
ls -la "$STAGE_DIR"
