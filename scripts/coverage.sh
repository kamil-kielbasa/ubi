#!/usr/bin/env bash
# Build and run UBI tests with code coverage, then generate HTML report.
# Usage: ./scripts/coverage.sh
# Requires: lcov, genhtml (from lcov package)
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"

BOARD="native_sim"
BUILD_DIR="${PROJECT_DIR}/build/${BOARD}/coverage"
COVERAGE_DIR="${PROJECT_DIR}/build/coverage"
OVERLAY_CONF="${PROJECT_DIR}/tests/boards/native_sim_coverage.conf"

echo "=== UBI Coverage: Building for ${BOARD} with coverage ==="
west build -p --build-dir "$BUILD_DIR" -b "$BOARD" "$PROJECT_DIR/tests/" \
    -- -DOVERLAY_CONFIG="$OVERLAY_CONF"

echo "=== UBI Coverage: Running tests ==="
"$BUILD_DIR/zephyr/zephyr.exe"

echo "=== UBI Coverage: Collecting coverage data ==="
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

echo "=== UBI Coverage: Generating HTML report ==="
genhtml "$COVERAGE_DIR/coverage_filtered.info" \
    --output-directory "$COVERAGE_DIR/html" \
    --branch-coverage \
    --title "UBI Code Coverage"

echo "=== UBI Coverage: Summary ==="
lcov --summary "$COVERAGE_DIR/coverage_filtered.info" --rc lcov_branch_coverage=1

echo ""
echo "HTML report: ${COVERAGE_DIR}/html/index.html"
