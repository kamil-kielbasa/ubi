#!/usr/bin/env bash
# Run UBI tests for a given board target.
# Usage: ./scripts/run_tests.sh [BOARD]
# Default board: native_sim
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"

BOARD="${1:-native_sim}"
BUILD_DIR="${PROJECT_DIR}/build/${BOARD}/tests"

echo "=== UBI Tests: Building for ${BOARD} ==="
west build -p --build-dir "$BUILD_DIR" -b "$BOARD" "$PROJECT_DIR/tests/"

if [ "$BOARD" = "native_sim" ]; then
    echo "=== UBI Tests: Running on ${BOARD} ==="
    "$BUILD_DIR/zephyr/zephyr.exe"
    echo "=== UBI Tests: PASSED ==="
else
    echo "=== UBI Tests: Build complete for ${BOARD} ==="
    echo "Flash with: STM32_Programmer_CLI -c port=SWD -d ${BUILD_DIR}/zephyr/zephyr.hex"
fi
