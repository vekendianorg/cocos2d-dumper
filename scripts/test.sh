#!/bin/sh
# Run the test suite.
#
#   sh scripts/test.sh              fast tests only (synthetic fixtures)
#   sh scripts/test.sh --real       also the real-binary tests
#
# The real-binary tests need STELLAR_REAL_BINARY to point at a large ELF; they skip
# themselves when it is unset, so the default run works anywhere.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
RUN_DIR="${STELLAR_RUN_DIR:-${TMPDIR:-/tmp}/stellar-run}"
T="$RUN_DIR/stellar-tests"
[ -x "$T" ] || T="$ROOT/build/tests/stellar-tests"

if [ "${1:-}" = "--real" ]; then
  exec "$T" --filter=RealBinary
fi
exec "$T"
