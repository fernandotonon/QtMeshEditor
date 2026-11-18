#!/usr/bin/env bash
# Benchmark the three crowd paths (skeletal / VAT / bone texture) at several
# instance counts on THIS machine. Each run opens a window, warms up 2 s,
# measures for SECONDS, prints one `BENCH {...}` JSON line and quits.
#
#   ./bench.sh                 # 100 400 1000 5000, 8 s each
#   COUNTS="1000" SECONDS_PER_RUN=15 ./bench.sh
#
# Results go to stdout as JSON lines (and bench_results.jsonl).
set -euo pipefail
cd "$(dirname "$0")"
GODOT="${GODOT:-godot}"
COUNTS="${COUNTS:-100 400 1000 5000}"
SECONDS_PER_RUN="${SECONDS_PER_RUN:-8}"
RES="${RES:-1280x720}"
OUT=bench_results.jsonl
: > "$OUT"
for n in $COUNTS; do
  for path in skeleton vat bta; do
    line=$("$GODOT" --path . "scenes/demo_perf_${path}.tscn" --resolution "$RES" \
             -- --n="$n" --bench="$SECONDS_PER_RUN" 2>/dev/null | grep '^BENCH ' | tail -1 || true)
    if [ -z "$line" ]; then
      echo "{\"path\":\"$path\",\"instances\":$n,\"error\":\"no BENCH line\"}" | tee -a "$OUT"
    else
      echo "${line#BENCH }" | tee -a "$OUT"
    fi
  done
done
