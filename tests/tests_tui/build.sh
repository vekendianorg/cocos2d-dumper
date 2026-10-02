#!/bin/sh
# Builds the frame-dump harness without CMake. Usage: tests_tui/build.sh [out]
cd "$(dirname "$0")/../.." || exit 1
SRCS=$(ls src/util/*.cpp src/diag/*.cpp src/elf/*.cpp src/dwarf/*.cpp src/ir/*.cpp src/output/*.cpp src/tui/*.cpp)
# The version is read from CMakeLists.txt rather than written out here. A second
# copy of the number is a second thing to forget at release time, and the CI
# "Verify version matches tag" job only guards CMakeLists.txt -- a stale copy
# here would silently build a binary that reports the wrong version.
VER=$(sed -n 's/^project(.* VERSION \([0-9.]*\)).*/\1/p' CMakeLists.txt)
VER_MAJOR=${VER%%.*}; VER_REST=${VER#*.}
VER_MINOR=${VER_REST%%.*}; VER_PATCH=${VER_REST#*.}
VDEF="-DSTELLAR_VERSION=\"$VER\" -DSTELLAR_VERSION_MAJOR=$VER_MAJOR"
VDEF="$VDEF -DSTELLAR_VERSION_MINOR=$VER_MINOR -DSTELLAR_VERSION_PATCH=$VER_PATCH"
g++ -std=c++20 -O1 -Wall -Wextra -Iinclude $VDEF $SRCS tests/tests_tui/frame_dump.cpp \
  -o "${1:-/tmp/frame_dump}" -pthread
