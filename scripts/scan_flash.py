#!/usr/bin/env python3
"""Scan a native_sim flash.bin for forbidden plaintext patterns.

Usage:
    python3 scripts/scan_flash.py [FLASH_BIN]

Default: flash.bin in the current directory.

Exit codes:
    0 — no forbidden patterns found
    1 — at least one forbidden pattern found
    2 — usage error

This is the host-side complement to the portable C forensic scan in
tests/src/secure/tests_ubi_secure_forensic.c.
"""

import re
import sys
from pathlib import Path

# ── Known patterns that must NEVER appear on a secure medium ──────────

# Test arrays from tests/src/common/arrays.h (first 16 bytes of array_128)
ARRAY_128_HEAD = bytes([
    0x94, 0x08, 0xE6, 0xE3, 0x32, 0x15, 0xF8, 0x80,
    0xF4, 0x85, 0x9D, 0xCA, 0x2C, 0xFD, 0x1E, 0xF6,
])

# Test root key material (all 0xAA, 16 bytes)
ROOT_KEY = b"\xAA" * 16

# ASCII volume names used in tests
VOLUME_NAMES = [
    b"/SECRET",
    b"/ubi_0",
]

# Printable ASCII strings that should not appear on encrypted media.
# Minimum 8 chars to reduce false positives.
ASCII_RE = re.compile(rb"[\x20-\x7e]{8,}")

# ── Scan logic ────────────────────────────────────────────────────────

RESERVED_PEB_COUNT = 2


def scan_flash(flash_path: Path, erase_block_size: int = 8192) -> list[dict]:
    """Scan flash.bin and return list of findings."""
    data = flash_path.read_bytes()
    findings: list[dict] = []

    # Skip reserved PEBs (first RESERVED_PEB_COUNT * erase_block_size bytes)
    data_start = RESERVED_PEB_COUNT * erase_block_size
    data_area = data[data_start:]

    # Check binary patterns
    patterns = [
        ("array_128 (first 16 bytes)", ARRAY_128_HEAD),
        ("root key material (0xAA * 16)", ROOT_KEY),
    ]
    for name in VOLUME_NAMES:
        patterns.append((f"volume name '{name.decode()}'", name))

    for label, pattern in patterns:
        offset = data_area.find(pattern)
        if offset >= 0:
            abs_offset = data_start + offset
            findings.append({
                "type": "binary_pattern",
                "label": label,
                "offset": abs_offset,
                "hex": data_area[offset : offset + len(pattern)].hex(),
            })

    # Check for suspicious ASCII strings in data area
    for m in ASCII_RE.finditer(data_area):
        text = m.group().decode("ascii", errors="replace")
        # Skip erased areas (all 0xFF is not printable, won't match)
        # Skip common non-secret patterns (UBI magic etc)
        if any(skip in text for skip in ["UBI#", "UBI!"]):
            continue
        abs_offset = data_start + m.start()
        findings.append({
            "type": "ascii_string",
            "label": f"ASCII: '{text[:60]}'",
            "offset": abs_offset,
            "hex": m.group()[:32].hex(),
        })

    return findings


def main() -> int:
    if len(sys.argv) > 2:
        print(f"Usage: {sys.argv[0]} [FLASH_BIN]", file=sys.stderr)
        return 2

    flash_path = Path(sys.argv[1]) if len(sys.argv) == 2 else Path("flash.bin")

    if not flash_path.exists():
        print(f"Error: {flash_path} not found", file=sys.stderr)
        print("Run native_sim tests first to generate the flash image.", file=sys.stderr)
        return 2

    print(f"Scanning {flash_path} ({flash_path.stat().st_size} bytes)...")
    findings = scan_flash(flash_path)

    if not findings:
        print("OK — no forbidden plaintext patterns found.")
        return 0

    print(f"\nFOUND {len(findings)} forbidden pattern(s):\n")
    for i, f in enumerate(findings, 1):
        print(f"  [{i}] {f['label']}")
        print(f"      offset: 0x{f['offset']:08x}")
        print(f"      hex:    {f['hex'][:64]}")
        print()

    return 1


if __name__ == "__main__":
    sys.exit(main())
