#!/bin/sh
# Produce a dump. Usage: sh scripts/emit.sh <elf> [output.cs]
#
# With no arguments it emits into output/dump.cs, or output/dump.dwarfless.cs
# when the input has no DWARF.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
RUN_DIR="${C2D_RUN_DIR:-${TMPDIR:-/tmp}/c2d-run}"
C2D="$RUN_DIR/c2d"
[ -x "$C2D" ] || C2D="$ROOT/build/c2d"

INPUT=${1:-}
OUTPUT=${2:-}
if [ -z "$INPUT" ]; then
  echo "usage: sh scripts/emit.sh <elf> [output.cs]" >&2
  exit 64
fi
cd "$ROOT"
if [ -n "$OUTPUT" ]; then
  exec "$C2D" emit -o "$OUTPUT" "$INPUT"
fi
exec "$C2D" emit "$INPUT"
