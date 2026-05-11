#!/usr/bin/env bash
# Keep pinned GitHub Action refs in docs aligned with project(QtMeshEditor VERSION …)
# in CMakeLists.txt. Floating refs (@v1, Docker :latest) are documented separately.
#
# Usage:
#   ./scripts/sync-doc-versions-from-cmake.sh          # apply in-repo edits
#   ./scripts/sync-doc-versions-from-cmake.sh --check  # exit 1 if anything would change
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CMAKE="$ROOT/CMakeLists.txt"

if [[ ! -f "$CMAKE" ]]; then
  echo "sync-doc-versions-from-cmake: missing $CMAKE" >&2
  exit 1
fi

VERSION="$(
  grep -E '^[[:space:]]*project\([[:space:]]*QtMeshEditor[[:space:]]+VERSION[[:space:]]+[0-9.]+' "$CMAKE" \
    | head -1 \
    | sed -E 's/.*VERSION[[:space:]]+([0-9.]+).*/\1/'
)"
if [[ -z "${VERSION}" ]] || [[ "${VERSION}" != *.*.* ]]; then
  echo "sync-doc-versions-from-cmake: could not parse X.Y.Z from $CMAKE" >&2
  exit 1
fi

EXPECTED="fernandotonon/QtMeshEditor@${VERSION}"
CHECK=0
if [[ "${1:-}" == "--check" ]]; then
  CHECK=1
fi

DOC_FILES=(
  "$ROOT/README.md"
  "$ROOT/website/src/hooks/useQtmeshActionRef.js"
)

verify_no_stale_pins() {
  local bad=0
  for f in "${DOC_FILES[@]}"; do
    [[ -f "$f" ]] || continue
    local refs
    refs="$(grep -ohE 'fernandotonon/QtMeshEditor@[0-9]+\.[0-9]+\.[0-9]+' "$f" 2>/dev/null | sort -u || true)"
    if [[ -z "${refs}" ]]; then
      continue
    fi
    while IFS= read -r r; do
      [[ -z "${r}" ]] && continue
      if [[ "${r}" != "${EXPECTED}" ]]; then
        echo "sync-doc-versions-from-cmake: ${f} contains pinned ref '${r}' (expected only '${EXPECTED}')" >&2
        bad=1
      fi
    done <<< "${refs}"
  done
  return "${bad}"
}

apply_perl_replace() {
  local f="$1"
  # Build replacement with q{} + concat so Perl never parses @3 in 3.0.0 as an array.
  QTMESH_DOC_VERSION="${VERSION}" perl -i -pe \
    'BEGIN { $v = $ENV{QTMESH_DOC_VERSION}; } s/fernandotonon\/QtMeshEditor@\d+\.\d+\.\d+/q{fernandotonon\/QtMeshEditor@} . $v/ge' \
    "$f"
}

if [[ "${CHECK}" -eq 1 ]]; then
  if ! verify_no_stale_pins; then
    echo "sync-doc-versions-from-cmake: run without --check to fix, or bump project() VERSION in CMakeLists.txt." >&2
    exit 1
  fi
  echo "sync-doc-versions-from-cmake: OK (pinned refs match CMakeLists.txt ${VERSION})"
  exit 0
fi

for f in "${DOC_FILES[@]}"; do
  if [[ -f "$f" ]] && grep -qE 'fernandotonon/QtMeshEditor@[0-9]+\.[0-9]+\.[0-9]+' "$f"; then
    apply_perl_replace "$f"
  fi
done

echo "sync-doc-versions-from-cmake: updated pinned refs to ${VERSION}"
