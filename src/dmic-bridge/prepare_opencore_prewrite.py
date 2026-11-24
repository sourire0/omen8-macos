#!/usr/bin/env python3
"""Create a disabled OpenCore review candidate without editing its source."""

from __future__ import annotations

import argparse
import hashlib
import plistlib
from pathlib import Path


def make_candidate_config(
    source_bytes: bytes, *, enabled: bool = False
) -> tuple[bytes, str]:
    config = plistlib.loads(source_bytes)
    try:
        entries = config["Kernel"]["Add"]
    except (KeyError, TypeError):
        raise ValueError("source config has no Kernel -> Add array")
    if not isinstance(entries, list):
        raise ValueError("Kernel -> Add must be an array")
    if not all(isinstance(entry, dict) for entry in entries):
        raise ValueError("every Kernel -> Add entry must be a dictionary")
    candidate_indexes = [
        index for index, entry in enumerate(entries)
        if entry.get("BundlePath") == "HPIPC4.kext"
    ]
    if len(candidate_indexes) > 1:
        raise ValueError("source contains duplicate HPIPC4.kext entries")
    probe_indexes = [
        index for index, entry in enumerate(entries)
        if entry.get("BundlePath") == "HPDMICProbe.kext"
    ]
    if len(probe_indexes) != 1:
        raise ValueError("expected exactly one HPDMICProbe.kext entry")

    candidate_entry = {
        "Arch": "x86_64",
        "BundlePath": "HPIPC4.kext",
        "Comment": (
            "Write review only; do not copy automatically to MULL EFI"
            if enabled else
            "Review candidate only; disabled pending signing and hardware validation"
        ),
        "Enabled": enabled,
        "ExecutablePath": "Contents/MacOS/HPIPC4",
        "MaxKernel": "",
        "MinKernel": "",
        "PlistPath": "Contents/Info.plist",
    }
    if candidate_indexes:
        candidate_index = candidate_indexes[0]
        existing = entries[candidate_index]
        for key, value in candidate_entry.items():
            existing[key] = value
    else:
        entries.insert(probe_indexes[0] + 1, candidate_entry)

    # The shared-open probe was only needed to establish that AppleHDA
    # tolerates a non-seizing PCI client. Once HPIPC4 is enabled, keep that
    # one-shot diagnostic from opening the same PCI function at boot.
    if enabled:
        shared_probe = [
            entry for entry in entries
            if entry.get("BundlePath") == "HPIPC4SharedOpenProbe.kext"
        ]
        if len(shared_probe) > 1:
            raise ValueError("source contains duplicate HPIPC4SharedOpenProbe.kext entries")
        if shared_probe:
            shared_probe[0]["Enabled"] = False

    digest = hashlib.sha256(source_bytes).hexdigest()
    return plistlib.dumps(config, fmt=plistlib.FMT_XML, sort_keys=False), digest


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("source_config", type=Path)
    parser.add_argument("output_config", type=Path)
    parser.add_argument("--enable-candidate", action="store_true")
    args = parser.parse_args()

    source = args.source_config.expanduser().resolve(strict=True)
    output = args.output_config.expanduser().resolve()
    if source == output:
        parser.error("output must differ from the source EFI config")

    try:
        candidate, digest = make_candidate_config(
            source.read_bytes(), enabled=args.enable_candidate
        )
    except (ValueError, plistlib.InvalidFileException) as error:
        parser.error(str(error))
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_bytes(candidate)
    print(f"source={source}")
    print(f"source_sha256={digest}")
    print(f"candidate={output}")
    print(f"candidate_entry_enabled={str(args.enable_candidate).lower()}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
