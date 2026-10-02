#!/bin/bash
# TUI layout + behaviour checks. No terminal and no CMake needed; exit status is
# non-zero on any failure.
set -eo pipefail
cd "$(dirname "$0")/../.."
# One read of the authoritative version, shared by every build below. Deriving it
# keeps this harness in step with CMakeLists.txt (see build.sh for why).
VER=$(sed -n 's/^project(.* VERSION \([0-9.]*\)).*/\1/p' CMakeLists.txt)
VER_MAJOR=${VER%%.*}; VER_REST=${VER#*.}
VER_MINOR=${VER_REST%%.*}; VER_PATCH=${VER_REST#*.}
VDEF="-DSTELLAR_VERSION=\"$VER\" -DSTELLAR_VERSION_MAJOR=$VER_MAJOR"
VDEF="$VDEF -DSTELLAR_VERSION_MINOR=$VER_MINOR -DSTELLAR_VERSION_PATCH=$VER_PATCH"
tests/tests_tui/build.sh /tmp/frame_dump
SRCS=$(ls src/util/*.cpp src/diag/*.cpp src/elf/*.cpp src/dwarf/*.cpp src/ir/*.cpp src/output/*.cpp src/tui/*.cpp src/app/main.cpp)
g++ -std=c++20 -O1 -Iinclude $VDEF $SRCS -o /tmp/stellar -pthread
echo "== size sweep 1x1..160x50, colour, all 8 screens";    /tmp/frame_dump 160 50 1 | python3 tests/tests_tui/vt.py
echo "== size sweep 1x1..100x30, no colour";                /tmp/frame_dump 100 30 0 | python3 tests/tests_tui/vt.py
echo "== hostile terminal (✓ ▶ ↑↓ — drawn double-width)";   /tmp/frame_dump 100 40 1 | AMBIG=1 python3 tests/tests_tui/vt.py
echo "== banner identity";                                  /tmp/frame_dump 120 45 1 | python3 tests/tests_tui/logo_check.py
echo "== real binary: live resize (pty)";                   python3 tests/tests_tui/pty_resize.py /tmp/stellar | tail -1
echo "== real binary: typing r/q/s in the input field";     python3 tests/tests_tui/pty_keys.py /tmp/stellar | tail -1
# A real ELF with DWARF, built on the spot, in a directory holding nothing else.
W=$(mktemp -d); cp tests/tests_tui/fixture.cpp "$W/t.cpp"
(cd "$W" && g++ -g -O0 t.cpp -o libdemo.so -shared -fPIC && rm t.cpp)
echo "== RAM limit is a real soft cap"
ROOT=$PWD
g++ -std=c++20 -O1 -Iinclude $VDEF \
  src/util/*.cpp src/diag/*.cpp src/elf/*.cpp src/dwarf/*.cpp src/ir/*.cpp src/output/*.cpp src/tui/analysis.cpp src/tui/dwarfview.cpp \
  tests/tests_tui/ram_cap.cpp -o /tmp/ram_cap -pthread
/tmp/ram_cap "$W/libdemo.so" "$W/cap.cs" | grep -E "^(PASS|FAIL)"
echo "== real binary: full flow on a real ELF (autofill, probe, browse, scan, emit)"
python3 tests/tests_tui/pty_flow.py /tmp/stellar "$W" | grep -E "FAIL|ALL OK|FAILED"
