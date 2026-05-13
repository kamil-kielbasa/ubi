#!/usr/bin/env python3
"""Check that every ZTEST() has a preceding docblock.

Required tags (errors — fail the run if missing):
    \\brief, \\details, \\expected

Optional tags (warnings — reported but never fail the run unless --strict):
    \\oracle       — the numeric / observational oracle that proves the test
    \\trace        — back-link to a spec section, requirement id, or PR
    \\precondition — non-obvious environmental setup the test relies on

Usage:
    python3 scripts/check_test_descriptions.py [DIR] [--strict]

Default DIR: tests/src/

Exit codes:
    0 — all tests have the required fields (warnings allowed unless --strict)
    1 — at least one test is missing a required field, or --strict and
        at least one optional tag is missing
"""

import re
import sys
from pathlib import Path

ZTEST_RE = re.compile(r"^\s*ZTEST\s*\(")
REQUIRED_TAGS = ("brief", "details", "expected")
OPTIONAL_TAGS = ("oracle", "trace", "precondition")
# Note: the project's docblocks historically use both `\expect` and `\expected`
# interchangeably; treat them as the same tag so legacy tests are not flagged.
_TAG_PATTERNS = {
    "brief": r"\\brief\b",
    "details": r"\\details\b",
    "expected": r"\\expect(?:ed)?\b",
    "oracle": r"\\oracle\b",
    "trace": r"\\trace\b",
    "precondition": r"\\precondition\b",
}
TAG_RE = {tag: re.compile(_TAG_PATTERNS[tag]) for tag in REQUIRED_TAGS + OPTIONAL_TAGS}


def check_file(path: Path) -> tuple[list[str], list[str]]:
    """Return (errors, warnings) for a single file."""
    errors: list[str] = []
    warnings: list[str] = []
    lines = path.read_text().splitlines()

    for i, line in enumerate(lines):
        if not ZTEST_RE.match(line):
            continue

        # Look backwards for the docblock (up to 50 lines).
        block_start = max(0, i - 50)
        docblock = "\n".join(lines[block_start:i])

        lineno = i + 1
        test_name = line.strip()

        for tag in REQUIRED_TAGS:
            if not TAG_RE[tag].search(docblock):
                errors.append(f"{path}:{lineno}: missing \\{tag} before {test_name}")
        for tag in OPTIONAL_TAGS:
            if not TAG_RE[tag].search(docblock):
                warnings.append(
                    f"{path}:{lineno}: missing \\{tag} (optional) before {test_name}"
                )

    return errors, warnings


def main() -> int:
    args = [a for a in sys.argv[1:] if a != "--strict"]
    strict = "--strict" in sys.argv[1:]

    search_dir = Path(args[0]) if args else Path("tests/src")

    if not search_dir.is_dir():
        print(f"Error: {search_dir} is not a directory", file=sys.stderr)
        return 2

    all_errors: list[str] = []
    all_warnings: list[str] = []

    for path in sorted(search_dir.rglob("*.c")):
        errs, warns = check_file(path)
        all_errors.extend(errs)
        all_warnings.extend(warns)

    if all_warnings:
        print(
            f"WARN — {len(all_warnings)} optional tag(s) missing "
            f"(\\oracle / \\trace / \\precondition); first 5 shown:",
            file=sys.stderr,
        )
        for warning in all_warnings[:5]:
            print(f"  {warning}", file=sys.stderr)

    if all_errors:
        print(f"FOUND {len(all_errors)} missing required description(s):\n")
        for issue in all_errors:
            print(f"  {issue}")
        return 1

    if strict and all_warnings:
        print(
            f"\n--strict: {len(all_warnings)} optional tag(s) missing "
            f"-> failing the run."
        )
        return 1

    count = sum(1 for _ in search_dir.rglob("*.c"))
    print(
        f"OK — all ZTEST() entries in {count} files have the required "
        f"\\brief / \\details / \\expected tags."
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
