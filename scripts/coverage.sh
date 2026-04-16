#!/usr/bin/env bash
# Build and run UBI tests with code coverage, then generate HTML report.
# Usage: ./scripts/coverage.sh [MODE]
#   MODE: plain (default), secure
# Requires: lcov, genhtml (from lcov package)
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"

MODE="${1:-plain}"
BOARD="native_sim"
BUILD_DIR="${PROJECT_DIR}/build/${BOARD}/coverage-${MODE}"
COVERAGE_DIR="${PROJECT_DIR}/build/coverage-${MODE}"

case "$MODE" in
    plain)
        OVERLAY_CONF="${PROJECT_DIR}/tests/boards/native_sim_coverage.conf"
        TITLE="UBI Plain Code Coverage"
        ;;
    secure)
        OVERLAY_CONF="${PROJECT_DIR}/tests/boards/native_sim_coverage_secure.conf"
        TITLE="UBI Secure Code Coverage"
        ;;
    *)
        echo "Error: unknown mode '$MODE' (use: plain, secure)" >&2
        exit 1
        ;;
esac

echo "=== UBI Coverage [${MODE}]: Building for ${BOARD} ==="
west build -p --build-dir "$BUILD_DIR" -b "$BOARD" "$PROJECT_DIR/tests/" \
    -- -DOVERLAY_CONFIG="$OVERLAY_CONF"

echo "=== UBI Coverage [${MODE}]: Running tests ==="
"$BUILD_DIR/zephyr/zephyr.exe"

echo "=== UBI Coverage [${MODE}]: Collecting coverage data ==="
mkdir -p "$COVERAGE_DIR"

lcov --capture \
    --directory "$BUILD_DIR" \
    --output-file "$COVERAGE_DIR/coverage.info" \
    --rc lcov_branch_coverage=1

# Filter to only UBI library sources
lcov --extract "$COVERAGE_DIR/coverage.info" \
    "*/lib/src/*" \
    --output-file "$COVERAGE_DIR/coverage_filtered.info" \
    --rc lcov_branch_coverage=1

echo "=== UBI Coverage [${MODE}]: Generating HTML report ==="
genhtml "$COVERAGE_DIR/coverage_filtered.info" \
    --output-directory "$COVERAGE_DIR/html" \
    --branch-coverage \
    --title "$TITLE"

echo "=== UBI Coverage [${MODE}]: Summary ==="
lcov --summary "$COVERAGE_DIR/coverage_filtered.info" --rc lcov_branch_coverage=1

echo ""
echo "HTML report: ${COVERAGE_DIR}/html/index.html"
