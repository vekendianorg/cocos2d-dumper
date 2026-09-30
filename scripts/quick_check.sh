#!/bin/sh
# Fast tests only: synthetic fixtures, no 583 MB input, a few seconds.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
RUN_DIR="${STELLAR_RUN_DIR:-/tmp/stellar-run}"
T="$RUN_DIR/stellar-tests"
[ -x "$T" ] || T="$ROOT/build/tests/stellar-tests"
exec "$T"
