#!/bin/bash

set -e

mode="${1:---fix}"

echo "Formatting (mode: $mode):"

find_sources() {
    find lib tests sample -type f \( -name '*.c' -o -name '*.h' \) | sort
}

if [ "$mode" = "--check" ]; then
    echo "- Checking formatting..."
    find_sources | xargs clang-format --dry-run --Werror
    echo "All files formatted correctly."
else
    echo "- Formatting all .c and .h files..."
    find_sources | xargs clang-format -i
    echo "Done."
fi
