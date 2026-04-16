#!/usr/bin/env python3
"""Check that every ZTEST() has a preceding brief/details/expected docblock.

Usage:
    python3 scripts/check_test_descriptions.py [DIR]

Default: tests/src/

Exit codes:
    0 — all tests have proper descriptions
    1 — at least one test is missing a description field
"""

import re
import sys
from pathlib import Path

ZTEST_RE = re.compile(r"^\s*ZTEST\s*\(")
BRIEF_RE = re.compile(r"\\brief\b")
DETAILS_RE = re.compile(r"\\details\b")
EXPECTED_RE = re.compile(r"\\expected\b")


def check_file(path: Path) -> list[str]:
    """Return list of issues for a single file."""
    issues: list[str] = []
    lines = path.read_text().splitlines()

    for i, line in enumerate(lines):
        if not ZTEST_RE.match(line):
            continue

        # Look backwards for the docblock (up to 30 lines).
        block_start = max(0, i - 30)
        docblock = "\n".join(lines[block_start:i])

        lineno = i + 1
        test_name = line.strip()

        if not BRIEF_RE.search(docblock):
            issues.append(f"{path}:{lineno}: missing \\brief before {test_name}")
        if not DETAILS_RE.search(docblock):
            issues.append(f"{path}:{lineno}: missing \\details before {test_name}")
        if not EXPECTED_RE.search(docblock):
            issues.append(f"{path}:{lineno}: missing \\expected before {test_name}")

    return issues


def main() -> int:
    search_dir = Path(sys.argv[1]) if len(sys.argv) > 1 else Path("tests/src")

    if not search_dir.is_dir():
        print(f"Error: {search_dir} is not a directory", file=sys.stderr)
        return 2

    all_issues: list[str] = []

    for path in sorted(search_dir.rglob("*.c")):
        all_issues.extend(check_file(path))

    if not all_issues:
        count = sum(1 for _ in search_dir.rglob("*.c"))
        print(f"OK — all ZTEST() entries in {count} files have brief/details/expected.")
        return 0

    print(f"FOUND {len(all_issues)} missing description(s):\n")
    for issue in all_issues:
        print(f"  {issue}")

    return 1


if __name__ == "__main__":
    sys.exit(main())
