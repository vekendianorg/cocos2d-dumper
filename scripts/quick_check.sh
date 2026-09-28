#!/bin/sh
# Fast tests only: synthetic fixtures, no 583 MB input, a few seconds.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
RUN_DIR="${C2D_RUN_DIR:-/tmp/c2d-run}"
T="$RUN_DIR/c2d-tests"
[ -x "$T" ] || T="$ROOT/build/tests/c2d-tests"
exec "$T"
