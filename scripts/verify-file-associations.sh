#!/usr/bin/env bash
# Smoke-check that file-association packaging files exist and list expected extensions.
# Full OS verification still requires manual VM tests (issue #669).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
FAIL=0

check_contains() {
  local file="$1"
  local needle="$2"
  if ! grep -Fq -- "$needle" "$file"; then
    echo "verify-file-associations: missing '$needle' in $file" >&2
    FAIL=1
  fi
}

echo "=== macOS Info.plist ==="
PLIST="$ROOT/src/Info.plist.in"
for ext in fbx glb gltf obj dae stl ply mesh rsd tmd; do
  check_contains "$PLIST" "<string>${ext}</string>"
done
check_contains "$PLIST" "CFBundleDocumentTypes"
check_contains "$PLIST" "UTExportedTypeDeclarations"

echo "=== Linux desktop + MIME ==="
DESKTOP="$ROOT/packaging/linux/qtmesheditor.desktop"
MIME="$ROOT/packaging/linux/qtmesheditor-mimetypes.xml"
check_contains "$DESKTOP" "Exec=qtmesheditor %F"
check_contains "$DESKTOP" "MimeType="
check_contains "$MIME" "application/x-ogre-mesh"
check_contains "$MIME" "application/vnd.ms-fbx"

echo "=== Windows packaging ==="
check_contains "$ROOT/packaging/windows/QtMeshEditor.iss" "OpenWithProgids"
check_contains "$ROOT/packaging/windows/QtMeshEditor.iss" "QtMeshEditor.Model.fbx"
check_contains "$ROOT/scripts/register-windows-file-associations.ps1" "OpenWithProgids"
check_contains "$ROOT/scripts/register-windows-file-associations.ps1" "QtMeshEditor.Model."

echo "=== Qt launch handler ==="
check_contains "$ROOT/src/AppLaunchHandler.h" "kServerName"
check_contains "$ROOT/src/main.cpp" "AppLaunchHandler"

if [[ "$FAIL" -ne 0 ]]; then
  exit 1
fi
echo "verify-file-associations: OK"
