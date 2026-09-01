#!/usr/bin/env bash
# Phase 13 CI gate: the TRELLIS.2 integration must never (re)introduce the
# prohibited NVIDIA research-only libraries (nvdiffrast / nvdiffrec — NVIDIA
# Source Code License, "research or evaluation purposes only"; see
# docs/trellis2-dependencies.md). Their names are allowed ONLY in license
# documentation, prohibition comments/guards, this script, and the guard test.
#
# Usage: ./scripts/check-trellis2-restricted-deps.sh   (from the repo root)
set -euo pipefail

cd "$(dirname "$0")/.."
fail=0

note() { echo "check-trellis2-restricted-deps: $*"; }

# Every scan target must exist and be readable — a missing path would make
# grep exit 2, which the `if` conditions below would treat exactly like
# "no match" and the gate would pass vacuously.
for target in ai/trellis2 src/ImageTo3D ai/trellis2/requirements.txt               ai/trellis2/install.py               src/ImageTo3D/Trellis2Predictor.cpp               src/ImageTo3D/Trellis2Bake.cpp               src/ImageTo3D/Trellis2Interchange.cpp               src/ImageTo3D/Trellis2Predictor.h               src/ImageTo3D/Trellis2Bake.h               src/ImageTo3D/Trellis2Interchange.h; do
    if [ ! -r "$target" ]; then
        note "FAIL: scan target missing/unreadable: $target"
        fail=1
    fi
done
[ "$fail" -ne 0 ] && { note "prohibited-dependency check FAILED"; exit 1; }

# 1. No import/require form ANYWHERE in the sidecar or the C++ integration.
#    (import nvdiffrast / from nvdiffrast import / import nvdiffrec…)
#    The guard test itself is an allowed location — it QUOTES the import forms
#    in order to detect them (same carve-out the spec gives this script).
if grep -RInE '(^|[^a-zA-Z_])(import|from)[[:space:]]+nvdiff(rast|rec)' \
        --exclude='Trellis2Guard_test.cpp' \
        ai/trellis2 src/ImageTo3D; then
    note "FAIL: an import of a prohibited NVIDIA library was introduced."
    fail=1
fi

# 2. Dependency manifests must not list them (or the PyPI `cumesh` trap — an
#    unrelated, unlicensed package; CuMesh is built from the pinned checkout).
#    Ban the names ANYWHERE in a non-comment line — editable/VCS forms
#    ("-e git+…#egg=nvdiffrast", "nvdiffrast @ git+…") don't start with the
#    package name.
if grep -v '^[[:space:]]*#' ai/trellis2/requirements.txt \
        | grep -inE 'nvdiffrast|nvdiffrec|cumesh'; then
    note "FAIL: a prohibited/trap package appears in requirements.txt."
    fail=1
fi

# 3. The C++ replacement layer may mention the names only in comments.
if grep -InE 'nvdiff(rast|rec)' \
        src/ImageTo3D/Trellis2Predictor.cpp \
        src/ImageTo3D/Trellis2Bake.cpp \
        src/ImageTo3D/Trellis2Interchange.cpp \
        src/ImageTo3D/Trellis2Predictor.h \
        src/ImageTo3D/Trellis2Bake.h \
        src/ImageTo3D/Trellis2Interchange.h \
    | grep -vE ':[0-9]+:[[:space:]]*(//|\*)' ; then
    note "FAIL: non-comment reference to a prohibited NVIDIA library in the C++ integration."
    fail=1
fi

# 4. install.py must not clone/install them either (git URLs, pip specs).
if grep -InE 'nvdiffrast\.git|nvdiffrec\.git|pip.*nvdiff' ai/trellis2/install.py; then
    note "FAIL: install.py fetches a prohibited NVIDIA library."
    fail=1
fi

if [ "$fail" -ne 0 ]; then
    note "prohibited-dependency check FAILED"
    exit 1
fi
note "OK — nvdiffrast/nvdiffrec are absent from the TRELLIS.2 integration."
