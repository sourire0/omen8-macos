#!/usr/bin/env python3
"""Fail closed unless a firmware file matches the reviewed complete-file digest."""

from __future__ import annotations

import hashlib
import sys
from pathlib import Path


def main() -> int:
    if len(sys.argv) != 3:
        print(f"usage: {sys.argv[0]} firmware.ri expected-sha256", file=sys.stderr)
        return 2

    firmware_path = Path(sys.argv[1])
    expected = sys.argv[2].lower()
    if len(expected) != 64 or any(c not in "0123456789abcdef" for c in expected):
        print("FAIL: expected digest must be 64 hexadecimal characters", file=sys.stderr)
        return 2

    try:
        actual = hashlib.sha256(firmware_path.read_bytes()).hexdigest()
    except OSError as exc:
        print(f"FAIL: cannot read firmware: {exc}", file=sys.stderr)
        return 1

    if actual != expected:
        print(f"FAIL: {firmware_path} SHA-256 {actual}; expected {expected}",
              file=sys.stderr)
        return 1

    print(f"PASS: {firmware_path} SHA-256 {actual}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
