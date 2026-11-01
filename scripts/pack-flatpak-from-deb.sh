#!/usr/bin/env bash
# Build a distributable Flatpak bundle from a QtMeshEditor Linux amd64 .deb.
# Usage:
#   ./scripts/pack-flatpak-from-deb.sh [/path/to/qtmesheditor_amd64.deb] [outdir]
#
# Requires: flatpak, flatpak-builder, and the org.freedesktop.{Platform,Sdk}//24.08
# runtimes (installed automatically on first build when network is available).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
FLATPAK_DIR="$ROOT/packaging/flatpak"
MANIFEST="$FLATPAK_DIR/io.github.fernandotonon.QtMeshEditor.yml"
APP_ID="io.github.fernandotonon.QtMeshEditor"
DEB="${1:-}"
OUTDIR="${2:-$ROOT}"

if [[ -z "$DEB" ]]; then
  if [[ -f "$ROOT/qtmesheditor_amd64.deb" ]]; then
    DEB="$ROOT/qtmesheditor_amd64.deb"
  elif [[ -f "$FLATPAK_DIR/qtmesheditor_amd64.deb" ]]; then
    DEB="$FLATPAK_DIR/qtmesheditor_amd64.deb"
  else
    echo "usage: $0 /path/to/qtmesheditor_amd64.deb [outdir]" >&2
    exit 2
  fi
fi
if [[ ! -f "$DEB" ]]; then
  echo "error: .deb not found: $DEB" >&2
  exit 1
fi

for tool in flatpak flatpak-builder; do
  command -v "$tool" >/dev/null || {
    echo "error: $tool not found" >&2
    exit 1
  }
done

VERSION="unknown"
if command -v dpkg-deb >/dev/null; then
  VERSION="$(dpkg-deb -f "$DEB" Version | sed 's/-.*//')"
else
  # ar+tar fallback when dpkg is missing (rare on Fedora builders).
  TMP_CTRL="$(mktemp -d)"
  (cd "$TMP_CTRL" && ar x "$DEB" control.tar.* >/dev/null && tar -xf control.tar.*)
  VERSION="$(awk -F': ' '/^Version:/{print $2; exit}' "$TMP_CTRL/control" | sed 's/-.*//')"
  rm -rf "$TMP_CTRL"
fi

# Stage the .deb next to the manifest (path: source in the yml).
cp -f "$DEB" "$FLATPAK_DIR/qtmesheditor_amd64.deb"

# Stamp AppStream release version + today's date.
META="$FLATPAK_DIR/io.github.fernandotonon.QtMeshEditor.metainfo.xml"
TODAY="$(date -u '+%Y-%m-%d')"
sed -i -E \
  "s|<release version=\"[0-9.]+\" date=\"[0-9-]+\"/>|<release version=\"${VERSION}\" date=\"${TODAY}\"/>|" \
  "$META"

BUILD_DIR="${FLATPAK_BUILD_DIR:-$ROOT/.flatpak-build}"
REPO_DIR="${FLATPAK_REPO_DIR:-$ROOT/.flatpak-repo}"
rm -rf "$BUILD_DIR"
mkdir -p "$BUILD_DIR" "$REPO_DIR" "$OUTDIR"

echo "==> flatpak-builder ($APP_ID $VERSION)"
# Install runtimes if missing (no-op when already present).
flatpak remote-add --user --if-not-exists flathub https://dl.flathub.org/repo/flathub.flatpakrepo || true
flatpak install -y --user flathub \
  org.freedesktop.Platform//24.08 \
  org.freedesktop.Sdk//24.08 || true

flatpak-builder --force-clean --user --install-deps-from=flathub \
  --repo="$REPO_DIR" \
  "$BUILD_DIR" \
  "$MANIFEST"

BUNDLE="$OUTDIR/${APP_ID}.flatpak"
echo "==> Exporting bundle $BUNDLE"
flatpak build-bundle "$REPO_DIR" "$BUNDLE" "$APP_ID" --runtime-repo=https://dl.flathub.org/repo/flathub.flatpakrepo

# Keep the working tree clean for developers; CI may still want the staged .deb.
if [[ "${KEEP_FLATPAK_DEB:-0}" != "1" ]]; then
  rm -f "$FLATPAK_DIR/qtmesheditor_amd64.deb"
fi

echo "Wrote $BUNDLE"
ls -lh "$BUNDLE"
echo
echo "Install with:"
echo "  flatpak install --user $BUNDLE"
echo "Run with:"
echo "  flatpak run $APP_ID"
echo "CLI:"
echo "  flatpak run --command=qtmesh $APP_ID --help"
