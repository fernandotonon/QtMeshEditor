#!/usr/bin/env bash
# Register qtmesh:// with a local QtMeshEditor build (development installs).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DESKTOP_TEMPLATE="$ROOT/packaging/linux/qtmesheditor-url.desktop"
APPS_DIR="${XDG_DATA_HOME:-$HOME/.local/share}/applications"
DESKTOP_PATH="$APPS_DIR/qtmesheditor-url.desktop"

if [[ ! -f "$DESKTOP_TEMPLATE" ]]; then
  echo "Missing desktop template: $DESKTOP_TEMPLATE" >&2
  exit 1
fi

BIN="$ROOT/build_local/bin/QtMeshEditor"
if [[ ! -x "$BIN" ]]; then
  BIN="$ROOT/build_local/bin/qtmesheditor"
fi
if [[ ! -x "$BIN" ]]; then
  echo "Build QtMeshEditor first (expected at build_local/bin/QtMeshEditor)." >&2
  exit 1
fi

mkdir -p "$APPS_DIR"
sed "s|^Exec=.*|Exec=$BIN %u|" "$DESKTOP_TEMPLATE" >"$DESKTOP_PATH"
chmod 644 "$DESKTOP_PATH"

if command -v update-desktop-database >/dev/null 2>&1; then
  update-desktop-database "$APPS_DIR" >/dev/null 2>&1 || true
fi
if command -v xdg-mime >/dev/null 2>&1; then
  xdg-mime default qtmesheditor-url.desktop x-scheme-handler/qtmesh
fi

echo "Registered qtmesh:// -> $BIN"
echo "Try: xdg-open 'qtmesh://cloud/open?owner=YOU&project=YOUR_PROJECT'"
