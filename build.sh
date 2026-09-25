#!/usr/bin/env bash
#
# Build convenience wrapper for the RP2350 PiZero VT100 terminal.
#
# Usage:
#   ./build.sh                       configure + build with defaults
#   ./build.sh -c                    delete the build directory first
#   ./build.sh /path/to/pico-sdk     point at a specific SDK checkout
#
# PICO_SDK_PATH is taken from the argument if given, otherwise from the
# environment. Nothing machine-specific is hardcoded.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="$ROOT/build"
PICO_SDK_PATH="${PICO_SDK_PATH:-}"
CLEAN=0

while [ $# -gt 0 ]; do
    case "$1" in
        -c|--clean) CLEAN=1; shift ;;
        -b|--build-dir) BUILD_DIR="$2"; shift 2 ;;
        -h|--help)
            grep '^#' "$0" | sed 's/^# \{0,1\}//'
            exit 0 ;;
        *) PICO_SDK_PATH="$1"; shift ;;
    esac
done

if [ -z "$PICO_SDK_PATH" ]; then
    echo "PICO_SDK_PATH is not set." >&2
    echo "Install the Raspberry Pi Pico SDK (2.x) and either export the" >&2
    echo "environment variable, or pass the path as an argument:" >&2
    echo "" >&2
    echo "    ./build.sh /path/to/pico-sdk" >&2
    exit 1
fi

if [ ! -f "$PICO_SDK_PATH/pico_sdk_init.cmake" ]; then
    echo "PICO_SDK_PATH '$PICO_SDK_PATH' does not look like a Pico SDK checkout." >&2
    exit 1
fi

if [ "$CLEAN" -eq 1 ] && [ -d "$BUILD_DIR" ]; then
    echo "Removing $BUILD_DIR"
    rm -rf "$BUILD_DIR"
fi

echo "Configuring ..."
cmake -S "$ROOT" -B "$BUILD_DIR" -G Ninja -DPICO_SDK_PATH="$PICO_SDK_PATH"

echo "Building ..."
cmake --build "$BUILD_DIR"

if [ -f "$BUILD_DIR/vt100_terminal.uf2" ]; then
    echo ""
    echo "Built $BUILD_DIR/vt100_terminal.uf2"
    echo "Hold BOOTSEL, plug the board in, then copy the UF2 to the RPI-RP2 drive."
else
    echo "Build finished but vt100_terminal.uf2 was not produced." >&2
fi
