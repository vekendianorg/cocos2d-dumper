#!/bin/sh
# Produce a dump. Usage: sh scripts/emit.sh <elf> [output.cs]
#
# With no arguments it emits into output/dump.cs, or output/dump.dwarfless.cs
# when the input has no DWARF.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
RUN_DIR="${STELLAR_RUN_DIR:-${TMPDIR:-/tmp}/stellar-run}"
STELLAR="$RUN_DIR/stellar"
[ -x "$STELLAR" ] || STELLAR="$ROOT/build/stellar"

INPUT=${1:-}
OUTPUT=${2:-}
if [ -z "$INPUT" ]; then
  echo "usage: sh scripts/emit.sh <elf> [output.cs]" >&2
  exit 64
fi
cd "$ROOT"
if [ -n "$OUTPUT" ]; then
  exec "$STELLAR" emit -o "$OUTPUT" "$INPUT"
fi
exec "$STELLAR" emit "$INPUT"
