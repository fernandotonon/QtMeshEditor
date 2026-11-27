#!/bin/sh
# Generic segmentation benchmark (#1179 slice 4, docs/GENERIC_SEGMENTATION_DESIGN.md).
#
#   scripts/genseg-benchmark.sh prepare <cc0-assets-dir> <bench-dir>
#   scripts/genseg-benchmark.sh run <bench-dir> <name>     (env knobs below)
#
# prepare: converts the CC0 Quaternius/Kenney models listed below to OBJ (the
# truth is their coloured pieces) and dumps the rig body labels of the rigged
# ones (<name>.rig.json: the truth for characters and animals, whose colour
# patches are not their parts). Nothing here is redistributed.
#
# run: the GenericSegmenter.Benchmark test (opt-in via its env vars) over every <bench-dir>/src/*.obj, in a
# low-poly and a dense (Loop-subdivided) variant, per granularity. SAM masks
# are cached in <bench-dir>/maskcache, so re-runs with other post-processing
# options take seconds. Results go to <bench-dir>/results/<name>.txt.
#   OPT="views=12,smoothness=0.2"   GenericSeg::Options overrides
#   GRAN=coarse,medium,fine  VARIANTS=low,dense  DUMP=1 (truth | prediction sheets)
#   QTMESH_SAM2_DIR=<graphs dir>    default: the app's model dir
set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${BIN:-$ROOT/build_local/bin}"
APP="${APP:-$BIN/QtMeshEditor.app/Contents/MacOS/QtMeshEditor}"
[ -x "$APP" ] || APP="$BIN/QtMeshEditor"

case "${1:-}" in
prepare)
    SRC="$2"; OUT="$3/src"; mkdir -p "$OUT"; FAILED=0
    for f in quaternius-men/Male_Suit quaternius-knight/KnightCharacter quaternius-robot/Robot \
             quaternius-alien/Alien quaternius-monsters/Skeleton quaternius-monsters/Dragon \
             quaternius-farm-animals/Horse quaternius-farm-animals/Cow quaternius-dinosaurs/Trex \
             quaternius-enemies/Spider quaternius-enemies/Frog quaternius-fish/Shark \
             quaternius-knight/Helmet1; do
        n=$(basename "$f")
        # Never leave a new OBJ next to stale or missing truth: convert into a
        # scratch folder under the final names and install the set together.
        STAGE="$OUT/.stage"; rm -rf "$STAGE"; mkdir -p "$STAGE"
        if ! "$APP" --cli convert "$SRC/$f.fbx" -o "$STAGE/$n.obj" >/dev/null 2>&1 || [ ! -s "$STAGE/$n.obj" ]; then
            echo "FAILED to convert $f (skipped)" >&2; FAILED=1; continue
        fi
        if [ "$n" = Helmet1 ]; then   # a prop: its pieces are the truth
            rm -f "$OUT/$n.rig.json"
        elif ! "$APP" --cli segment "$SRC/$f.fbx" --dump-training-data "$STAGE/$n.rig.json" >/dev/null 2>&1 \
             || [ ! -s "$STAGE/$n.rig.json" ]; then
            echo "FAILED to extract the rig truth of $f (skipped)" >&2; FAILED=1; continue
        fi
        mv "$STAGE"/* "$OUT/"
    done
    for n in Pistol SniperRifle; do cp "$SRC/quaternius-guns/$n.obj" "$SRC/quaternius-guns/$n.mtl" "$OUT/"; done
    for n in a e h; do cp "$SRC/kenney-blocky/character-$n.obj" "$SRC/kenney-blocky/character-$n.mtl" "$OUT/"; done
    rm -rf "$OUT/.stage"
    ls "$OUT"/*.obj | wc -l
    exit "$FAILED"
    ;;
run)
    B="$2"; NAME="$3"; mkdir -p "$B/results"
    export QTMESH_TESTS_SKIP_OGRE_PREFLIGHT=1 QT_QPA_PLATFORM=offscreen
    export QTMESH_GENSEG_BENCH="$B/src" QTMESH_GENSEG_MASKCACHE="$B/maskcache"
    export QTMESH_SAM2_DIR="${QTMESH_SAM2_DIR:-download}"
    export QTMESH_GENSEG_VARIANTS="${VARIANTS:-low,dense}" QTMESH_GENSEG_GRAN="${GRAN:-coarse,medium,fine}"
    export QTMESH_GENSEG_OPT="${OPT:-}"
    if [ -n "${DUMP:-}" ]; then export QTMESH_GENSEG_DUMP="$B/dump/$NAME"; fi
    RAW="$B/results/$NAME.log"
    STATUS=0
    "$BIN/UnitTests" --gtest_filter='GenericSegmenter.Benchmark' \
        > "$RAW" 2>&1 || STATUS=$?
    if [ "$STATUS" -ne 0 ]; then
        echo "benchmark failed (exit $STATUS); full log: $RAW" >&2
        tail -20 "$RAW" >&2
        exit "$STATUS"
    fi
    grep -vE '^\[|^Note|^UnitTests|^$|onnxruntime' "$RAW" > "$B/results/$NAME.txt"
    rm -f "$RAW"
    grep -E '^  [a-z]+ [a-z]+ +tIoU|^SAM' "$B/results/$NAME.txt"
    ;;
*)
    sed -n '2,20p' "$0"; exit 2 ;;
esac
