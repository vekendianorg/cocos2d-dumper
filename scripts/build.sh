#!/bin/sh
# Configure and build stellar on Linux, macOS, Termux (native or proot) and MSYS2.
#
# On Windows use CMakePresets.json from a Developer Command Prompt instead:
#     cmake --preset release && cmake --build --preset release
#
# The Termux preset exists because CMake's host detection shells out to
# `getprop`, which does not exist under proot, so it cannot determine the system
# version and aborts. Supplying CMAKE_SYSTEM_NAME/VERSION skips that path and is
# harmless on native Termux.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
RUN_DIR="${STELLAR_RUN_DIR:-${TMPDIR:-/tmp}/stellar-run}"
BUILD="${STELLAR_BUILD_DIR:-$ROOT/build}"

# Detect Termux so the workaround is applied only where it is needed.
#
# PREFIX is not exported under proot, so the install path is the reliable signal.
# Set STELLAR_FORCE_TERMUX=1 to apply the workaround on any host, or
# STELLAR_FORCE_TERMUX=0 to skip it on Termux.
TERMUX=0
if [ -d /data/data/com.termux/files/usr ]; then TERMUX=1; fi
if [ -n "${STELLAR_FORCE_TERMUX:-}" ]; then TERMUX="$STELLAR_FORCE_TERMUX"; fi

if [ "$TERMUX" = "1" ]; then
  echo "Termux detected; using the Termux CMake preset settings"
  cmake -S "$ROOT" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_SYSTEM_NAME=Linux -DCMAKE_SYSTEM_VERSION=24
else
  cmake -S "$ROOT" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release
fi
ninja -C "$BUILD"

# Some filesystems (notably Android's sdcardfs) do not carry the executable bit,
# so the binaries are copied somewhere that does before being run.
mkdir -p "$RUN_DIR"
cp "$BUILD/stellar" "$RUN_DIR/stellar"
cp "$BUILD/tests/stellar-tests" "$RUN_DIR/stellar-tests"
chmod +x "$RUN_DIR/stellar" "$RUN_DIR/stellar-tests" 2>/dev/null || true
echo "built: $RUN_DIR/stellar, $RUN_DIR/stellar-tests"
