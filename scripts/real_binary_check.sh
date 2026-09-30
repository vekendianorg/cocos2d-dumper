#!/bin/sh
# Run the real-binary tests (opt-in: they open the 583 MB input).
#
# Prefers the binary copied to a real filesystem by build.sh, because
# /storage/emulated/0 (sdcardfs) does not carry the executable bit and both a
# direct exec and CTest fail there with EACCES.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
RUN_DIR="${STELLAR_RUN_DIR:-/tmp/stellar-run}"

T="$RUN_DIR/stellar-tests"
if [ -x "$T" ]; then
  exec "$T" --filter=RealBinary
fi
if command -v ctest >/dev/null 2>&1 && [ -d "$ROOT/build" ]; then
  exec ctest --test-dir "$ROOT/build" -R stellar_real_binary --output-on-failure
fi
echo "stellar-tests not found; run scripts/build.sh first" >&2
exit 1
