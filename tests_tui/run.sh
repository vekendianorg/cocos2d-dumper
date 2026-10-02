#!/bin/sh
# TUI layout checks: no terminal and no CMake needed.
# Exit status is non-zero on any failure.
#
#   tests_tui/run.sh [maxcols maxrows]
#
# Defaults sweep every size from 1x1 to 200x60. That is ~72k frames per pass and
# takes a couple of minutes, so pass a smaller bound while iterating.
set -e
cd "$(dirname "$0")/.."
MAXC=${1:-200}
MAXR=${2:-60}
sh tests_tui/build.sh /tmp/frame_dump

echo "== size sweep 1x1..${MAXC}x${MAXR}, colour"
/tmp/frame_dump "$MAXC" "$MAXR" 1 | python3 tests_tui/vt.py

echo "== size sweep 1x1..${MAXC}x${MAXR}, no colour"
/tmp/frame_dump "$MAXC" "$MAXR" 0 | python3 tests_tui/vt.py

echo "== hostile terminal (check marks, arrows and dashes drawn double-width)"
/tmp/frame_dump 120 45 1 | AMBIG=1 python3 tests_tui/vt.py

echo "== banner identity and centring"
/tmp/frame_dump 120 45 1 | python3 tests_tui/logo_check.py

echo "== real binary: live resize (pty)"
SRCS=$(ls src/util/*.cpp src/diag/*.cpp src/elf/*.cpp src/dwarf/*.cpp src/ir/*.cpp src/output/*.cpp src/tui/*.cpp src/app/main.cpp)
g++ -std=c++20 -O1 -Iinclude -DSTELLAR_VERSION='"1.1.1"' -DSTELLAR_VERSION_MAJOR=1 \
  -DSTELLAR_VERSION_MINOR=1 -DSTELLAR_VERSION_PATCH=1 $SRCS -o /tmp/stellar -pthread
# No pipe: a pipeline would report tail's status and swallow a failure.
python3 tests_tui/pty_resize.py /tmp/stellar

echo "== real binary: r/q/s are text in the input field, shortcuts only in the menu (pty)"
python3 tests_tui/pty_keys.py /tmp/stellar

echo "ALL LAYOUT CHECKS PASSED"
