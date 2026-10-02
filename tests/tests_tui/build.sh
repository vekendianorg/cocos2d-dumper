#!/bin/sh
# Builds the frame-dump harness without CMake. Usage: tests_tui/build.sh [out]
cd "$(dirname "$0")/.." || exit 1
SRCS=$(ls src/util/*.cpp src/diag/*.cpp src/elf/*.cpp src/dwarf/*.cpp src/ir/*.cpp src/output/*.cpp src/tui/*.cpp)
g++ -std=c++20 -O1 -Wall -Wextra -Iinclude -DSTELLAR_VERSION='"1.1.1"' -DSTELLAR_VERSION_MAJOR=1 \
  -DSTELLAR_VERSION_MINOR=1 -DSTELLAR_VERSION_PATCH=1 $SRCS tests_tui/frame_dump.cpp \
  -o "${1:-/tmp/frame_dump}" -pthread
