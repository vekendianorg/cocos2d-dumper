#!/bin/sh
# Run the real-binary tests (opt-in: they open the 583 MB input).
#
# Prefers the binary copied to a real filesystem by build.sh, because
# /storage/emulated/0 (sdcardfs) does not carry the executable bit and both a
# direct exec and CTest fail there with EACCES.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
RUN_DIR="${C2D_RUN_DIR:-/tmp/c2d-run}"

T="$RUN_DIR/c2d-tests"
if [ -x "$T" ]; then
  exec "$T" --filter=RealBinary
fi
if command -v ctest >/dev/null 2>&1 && [ -d "$ROOT/build" ]; then
  exec ctest --test-dir "$ROOT/build" -R c2d_real_binary --output-on-failure
fi
echo "c2d-tests not found; run scripts/build.sh first" >&2
exit 1
