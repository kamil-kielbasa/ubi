#!/usr/bin/env bash
# Run UBI tests for a given board and mode.
# Usage: ./scripts/run_tests.sh [BOARD] [MODE]
#   BOARD: native_sim (default), b_u585i_iot02a, nrf5340dk/nrf5340/cpuapp
#   MODE:  plain (default), secure, chunked
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"

BOARD="${1:-native_sim}"
MODE="${2:-plain}"
BUILD_DIR="${PROJECT_DIR}/build/${BOARD}/${MODE}"

# Select overlay config based on mode.
OVERLAY_ARGS=""
case "$MODE" in
    plain)
        ;;
    secure)
        OVERLAY_ARGS="-DOVERLAY_CONFIG=boards/native_sim_secure.conf"
        ;;
    chunked)
        OVERLAY_ARGS="-DOVERLAY_CONFIG=boards/native_sim_secure_chunked.conf"
        ;;
    *)
        echo "Error: unknown mode '$MODE' (use: plain, secure, chunked)" >&2
        exit 1
        ;;
esac

echo "=== UBI Tests: Building for ${BOARD} [${MODE}] ==="
if [ -n "$OVERLAY_ARGS" ]; then
    west build -p --build-dir "$BUILD_DIR" -b "$BOARD" "$PROJECT_DIR/tests/" \
        -- -DCONFIG_FLASH_SIMULATOR=y $OVERLAY_ARGS
else
    west build -p --build-dir "$BUILD_DIR" -b "$BOARD" "$PROJECT_DIR/tests/" \
        -- -DCONFIG_FLASH_SIMULATOR=y
fi

if [ "$BOARD" = "native_sim" ]; then
    echo "=== UBI Tests: Running on ${BOARD} [${MODE}] ==="
    "$BUILD_DIR/zephyr/zephyr.exe"
    echo "=== UBI Tests: PASSED [${MODE}] ==="
else
    echo "=== UBI Tests: Build complete for ${BOARD} [${MODE}] ==="
    echo "Flash with: west flash --build-dir ${BUILD_DIR}"
fi
