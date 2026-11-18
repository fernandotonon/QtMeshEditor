#!/usr/bin/env bash
# Benchmark the three crowd paths (skeletal / VAT / bone texture) at several
# instance counts on THIS machine. Each run opens a window, warms up 2 s,
# measures for SECONDS, prints one `BENCH {...}` JSON line and quits.
#
#   ./bench.sh                 # 100 400 1000 5000, 8 s each
#   COUNTS="1000" SECONDS_PER_RUN=15 ./bench.sh
#
# Results go to stdout as JSON lines (and bench_results.jsonl). One sample
# per path and count: treat small differences as noise.
set -euo pipefail
cd "$(dirname "$0")"
GODOT="${GODOT:-godot}"
COUNTS="${COUNTS:-100 400 1000 5000}"
SECONDS_PER_RUN="${SECONDS_PER_RUN:-8}"
RES="${RES:-1280x720}"
OUT=bench_results.jsonl
: > "$OUT"
# The path order rotates with each instance count so no path always runs
# first (cold) or last (warm). Each run records its slot in "order".
PATHS=(skeleton vat bta)
rot=0
for n in $COUNTS; do
  for k in 0 1 2; do
    path=${PATHS[$(( (k + rot) % 3 ))]}
    log=$(mktemp)
    set +e
    "$GODOT" --path . "scenes/demo_perf_${path}.tscn" --resolution "$RES" \
      -- --n="$n" --bench="$SECONDS_PER_RUN" >"$log" 2>&1
    status=$?
    set -e
    line=$(grep '^BENCH ' "$log" | tail -1 || true)
    if [ $status -ne 0 ] || [ -z "$line" ]; then
      reason=$(grep -E 'BENCH_SETUP_FAILED|ERROR' "$log" | head -3 | tr '"' "'" | tr '\n' ' ')
      echo "{\"path\":\"$path\",\"instances\":$n,\"order\":$k,\"error\":\"exit $status: $reason\"}" | tee -a "$OUT"
    else
      echo "${line#BENCH }" | sed "s/^{/{\"order\":$k,/" | tee -a "$OUT"
    fi
    rm -f "$log"
  done
  rot=$(( rot + 1 ))
done
