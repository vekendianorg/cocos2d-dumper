#!/bin/sh
# Benchmark c2d against the real target binary.
# Prints measured counts, throughput and peak RSS (kernel VmHWM).
set -eu

BIN="${1:-libcocos2dcpp_1.74.2.so}"
C2D="${C2D_RUN_DIR:-/tmp/c2d-run}/c2d"
[ -x "$C2D" ] || C2D=build/c2d

echo "=== info ==="
"$C2D" info "$BIN"

echo
echo "=== bounded scans (cheap iteration) ==="
/usr/bin/env time -f '  wall=%es maxrss=%MkB' "$C2D" scan --max-units 20 --stats "$BIN" 2>&1 \
  | grep -E 'scanned DIEs|elapsed|wall=' || true
/usr/bin/env time -f '  wall=%es maxrss=%MkB' "$C2D" scan --unit-stride 100 --stats "$BIN" 2>&1 \
  | grep -E 'scanned DIEs|elapsed|wall=' || true

echo
echo "=== full scan, 3 runs ==="
i=1
while [ "$i" -le 3 ]; do
  echo "-- run $i --"
  "$C2D" scan --stats "$BIN" | grep -E 'scanned units|scanned DIEs|max depth|elapsed|peak_rss|wall_seconds'
  i=$((i + 1))
done
