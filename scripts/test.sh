#!/bin/sh
# Run the test suite.
#
#   sh scripts/test.sh              fast tests only (synthetic fixtures)
#   sh scripts/test.sh --real       also the real-binary tests
#
# The real-binary tests need C2D_REAL_BINARY to point at a large ELF; they skip
# themselves when it is unset, so the default run works anywhere.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
RUN_DIR="${C2D_RUN_DIR:-${TMPDIR:-/tmp}/c2d-run}"
T="$RUN_DIR/c2d-tests"
[ -x "$T" ] || T="$ROOT/build/tests/c2d-tests"

if [ "${1:-}" = "--real" ]; then
  exec "$T" --filter=RealBinary
fi
exec "$T"
