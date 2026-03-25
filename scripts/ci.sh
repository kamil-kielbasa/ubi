#!/usr/bin/env bash
# CI pipeline: build for all targets, run tests on native_sim.
# Usage: ./scripts/ci.sh [EXTRA_HW_TARGETS...]
# Default targets: native_sim b_u585i_iot02a
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"

HW_TARGETS="${@:-b_u585i_iot02a}"

echo "=== UBI CI Pipeline ==="

# 1. Build and run tests on native_sim
echo ""
echo "--- [native_sim] Build & Run Tests ---"
"$SCRIPT_DIR/run_tests.sh" native_sim

# 2. Build sample for native_sim
echo ""
echo "--- [native_sim] Build Sample ---"
west build -p --build-dir "$PROJECT_DIR/build/native_sim/sample" \
    -b native_sim "$PROJECT_DIR/sample/"

# 3. Build for hardware targets (compile-only, no run)
for target in $HW_TARGETS; do
    echo ""
    echo "--- [${target}] Build Tests ---"
    west build -p --build-dir "$PROJECT_DIR/build/${target}/tests" \
        -b "$target" "$PROJECT_DIR/tests/"

    echo ""
    echo "--- [${target}] Build Sample ---"
    west build -p --build-dir "$PROJECT_DIR/build/${target}/sample" \
        -b "$target" "$PROJECT_DIR/sample/"
done

echo ""
echo "=== CI Pipeline Complete ==="
