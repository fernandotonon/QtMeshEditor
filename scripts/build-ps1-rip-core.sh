#!/usr/bin/env bash
# Build the rip-instrumented beetle-psx fork (issue #813) and install it into
# QtMeshEditor PS1Cores/ as beetle_psx_qtmesh_libretro.<ext>.
#
# The fork lives at fernandotonon/beetle-psx-libretro, branch qtmesh-rip. The
# build is pinned to QTMESH_RIP_CORE_COMMIT below — bump it deliberately when
# the fork advances (CI caches on this value; an upstream rebase must never
# silently change capture semantics, #817).
#
# Usage: ./scripts/build-ps1-rip-core.sh [path/to/output/PS1Cores]
# Env:
#   QTMESH_RIP_CORE_REPO   override the clone URL (e.g. a local path)
#   QTMESH_RIP_CORE_COMMIT override the pinned commit (testing only)
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT_DIR="${1:-$ROOT/build/debug/PS1Cores}"

REPO_URL="${QTMESH_RIP_CORE_REPO:-https://github.com/fernandotonon/beetle-psx-libretro.git}"
# Pinned fork commit (branch qtmesh-rip): rip ABI v1 + GTE/GP0 capture hooks.
PINNED_COMMIT="${QTMESH_RIP_CORE_COMMIT:-924c475c48513cc2c8722fb789d1a86c3677318f}"

WORK_DIR="${QTMESH_RIP_CORE_WORKDIR:-$ROOT/build/beetle-psx-qtmesh}"

case "$(uname -s)" in
  Darwin)
    PLATFORM=osx
    EXT=dylib
    ;;
  Linux)
    PLATFORM=unix
    EXT=so
    ;;
  MINGW*|MSYS*|CYGWIN*)
    PLATFORM=win
    EXT=dll
    ;;
  *)
    echo "build-ps1-rip-core: unsupported platform $(uname -s)" >&2
    exit 1
    ;;
esac

ARTIFACT="beetle_psx_qtmesh_libretro.$EXT"

# Fast path: artifact already present and built from the pinned commit.
STAMP="$WORK_DIR/.qtmesh-rip-commit"
if [[ -f "$OUT_DIR/$ARTIFACT" && -f "$STAMP" ]] \
   && [[ "$(cat "$STAMP")" == "$PINNED_COMMIT" ]]; then
  echo "build-ps1-rip-core: $ARTIFACT already built at $PINNED_COMMIT — skipping"
  exit 0
fi

if [[ ! -d "$WORK_DIR/.git" ]]; then
  git clone "$REPO_URL" "$WORK_DIR"
fi

git -C "$WORK_DIR" fetch origin qtmesh-rip
git -C "$WORK_DIR" checkout --detach "$PINNED_COMMIT"

JOBS="$( (command -v nproc >/dev/null && nproc) || sysctl -n hw.ncpu 2>/dev/null || echo 4)"

echo "build-ps1-rip-core: building $ARTIFACT (platform=$PLATFORM, $JOBS jobs, commit $PINNED_COMMIT)"
make -C "$WORK_DIR" -j"$JOBS" HAVE_QTMESH_RIP=1 platform="$PLATFORM"

mkdir -p "$OUT_DIR"
cp -f "$WORK_DIR/$ARTIFACT" "$OUT_DIR/"
echo "$PINNED_COMMIT" > "$STAMP"
echo "build-ps1-rip-core: installed $ARTIFACT -> $OUT_DIR/"
