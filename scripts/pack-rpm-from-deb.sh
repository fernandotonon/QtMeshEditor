#!/usr/bin/env bash
# Build a Fedora/RHEL .rpm from a QtMeshEditor Linux .deb (release artifact).
# Usage:
#   ./scripts/pack-rpm-from-deb.sh [/path/to/qtmesheditor_amd64.deb] [outdir]
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEB="${1:-}"
OUTDIR="${2:-$ROOT}"

if [[ -z "$DEB" ]]; then
  if [[ -f "$ROOT/qtmesheditor_amd64.deb" ]]; then
    DEB="$ROOT/qtmesheditor_amd64.deb"
  else
    echo "usage: $0 /path/to/qtmesheditor_{amd64,arm64}.deb [outdir]" >&2
    exit 2
  fi
fi
if [[ ! -f "$DEB" ]]; then
  echo "error: .deb not found: $DEB" >&2
  exit 1
fi

command -v rpmbuild >/dev/null || {
  echo "error: rpmbuild not found (install rpm / rpm-build)" >&2
  exit 1
}
command -v dpkg-deb >/dev/null || {
  echo "error: dpkg-deb not found (needed to read the .deb)" >&2
  exit 1
}

VERSION="$(dpkg-deb -f "$DEB" Version | sed 's/-.*//')"
DEB_ARCH="$(dpkg-deb -f "$DEB" Architecture)"
case "$DEB_ARCH" in
  amd64) RPM_ARCH=x86_64 ;;
  arm64) RPM_ARCH=aarch64 ;;
  *)
    echo "error: unsupported .deb architecture: $DEB_ARCH" >&2
    exit 1
    ;;
esac

WORKDIR="$(mktemp -d "${TMPDIR:-/tmp}/qtmesh-rpm.XXXXXX")"
cleanup() { rm -rf "$WORKDIR"; }
trap cleanup EXIT

PAYLOAD="$WORKDIR/BUILD/payload"
mkdir -p "$PAYLOAD" "$WORKDIR/SPECS" "$WORKDIR/RPMS"

echo "==> Extracting $DEB"
dpkg-deb -x "$DEB" "$PAYLOAD"

# Icons (deb currently has no hicolor icons — desktop Icon=qtmesheditor).
ICON_SRC="$ROOT/packaging/linux/qtmesheditor.png"
if [[ ! -f "$ICON_SRC" ]]; then
  ICON_SRC="$ROOT/packaging/flatpak/icons/hicolor/256x256/apps/io.github.fernandotonon.QtMeshEditor.png"
fi
for size in 16 32 48 64 128 256 512; do
  dest="$PAYLOAD/usr/share/icons/hicolor/${size}x${size}/apps"
  mkdir -p "$dest"
  if command -v python3 >/dev/null && python3 -c 'from PIL import Image' 2>/dev/null; then
    python3 - "$ICON_SRC" "$dest/qtmesheditor.png" "$size" <<'PY'
import sys
from PIL import Image
src, dst, size = sys.argv[1], sys.argv[2], int(sys.argv[3])
Image.open(src).convert("RGBA").resize((size, size), Image.Resampling.LANCZOS).save(dst)
PY
  else
    # Fallback: install the source PNG at every size (desktop still finds it).
    cp "$ICON_SRC" "$dest/qtmesheditor.png"
  fi
done

# AppStream metainfo (same id as Flatpak).
META_SRC="$ROOT/packaging/flatpak/io.github.fernandotonon.QtMeshEditor.metainfo.xml"
mkdir -p "$PAYLOAD/usr/share/metainfo"
sed "s/version=\"[0-9.]*\"/version=\"${VERSION}\"/" "$META_SRC" \
  > "$PAYLOAD/usr/share/metainfo/io.github.fernandotonon.QtMeshEditor.metainfo.xml"

# Ensure desktop files + MIME from repo packaging (authoritative).
install -D -m644 "$ROOT/packaging/linux/qtmesheditor.desktop" \
  "$PAYLOAD/usr/share/applications/qtmesheditor.desktop"
install -D -m644 "$ROOT/packaging/linux/qtmesheditor-url.desktop" \
  "$PAYLOAD/usr/share/applications/qtmesheditor-url.desktop"
install -D -m644 "$ROOT/packaging/linux/qtmesheditor-mimetypes.xml" \
  "$PAYLOAD/usr/share/mime/packages/qtmesheditor-mimetypes.xml"

CHANGELOG_DATE="$(date '+%a %b %d %Y')"
SPEC="$WORKDIR/SPECS/qtmesheditor.spec"
sed \
  -e "s/@VERSION@/${VERSION}/g" \
  -e "s/@RPM_ARCH@/${RPM_ARCH}/g" \
  -e "s/@CHANGELOG_DATE@/${CHANGELOG_DATE}/g" \
  "$ROOT/packaging/fedora/qtmesheditor.spec.in" > "$SPEC"

echo "==> rpmbuild -bb ($VERSION $RPM_ARCH)"
rpmbuild -bb \
  --define "_topdir $WORKDIR" \
  --define "_builddir $WORKDIR/BUILD" \
  --define "_rpmdir $WORKDIR/RPMS" \
  --define "_specdir $WORKDIR/SPECS" \
  --define "dist .fc" \
  "$SPEC"

mkdir -p "$OUTDIR"
RPM_OUT="$(find "$WORKDIR/RPMS" -type f -name '*.rpm' | head -n1)"
if [[ -z "$RPM_OUT" ]]; then
  echo "error: rpmbuild produced no .rpm" >&2
  exit 1
fi
FINAL="$OUTDIR/qtmesheditor_${DEB_ARCH}.rpm"
cp -f "$RPM_OUT" "$FINAL"
echo "Wrote $FINAL"
ls -lh "$FINAL"
