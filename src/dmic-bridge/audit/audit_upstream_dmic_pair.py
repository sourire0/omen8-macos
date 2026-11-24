#!/usr/bin/env python3
"""Cross-reference an offline SOF IPC4 firmware image and topology by UUID."""

from __future__ import annotations

import argparse
import hashlib
import importlib.util
import re
import struct
import sys
import uuid
from pathlib import Path


def load_parser_module(script_dir: Path):
    path = script_dir / "audit_oem_ipc4.py"
    spec = importlib.util.spec_from_file_location("audit_oem_ipc4", path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load parser module: {path}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("firmware", type=Path)
    parser.add_argument("topology", type=Path)
    args = parser.parse_args()
    try:
        audit = load_parser_module(Path(__file__).resolve().parent)
        fw_data = args.firmware.read_bytes()
        tplg_data = args.topology.read_bytes()
        fw = audit.parse_firmware(args.firmware.name, fw_data)
    except (OSError, ValueError, RuntimeError) as exc:
        print(f"audit error: {exc}", file=sys.stderr)
        return 2

    strings = [
        (match.start(), match.group().decode("ascii", "replace"))
        for match in re.finditer(rb"[\x20-\x7e]{4,}", tplg_data)
    ]
    print(
        f"firmware={fw.name} version={'.'.join(map(str, fw.version))} "
        f"sha256={fw.sha256} modules={len(fw.modules)}"
    )
    print(
        f"topology={args.topology.name} bytes={len(tplg_data)} "
        f"sha256={hashlib.sha256(tplg_data).hexdigest()}"
    )
    print("Firmware UUIDs present in topology:")
    for module in fw.modules:
        needle = uuid.UUID(module.module_uuid).bytes_le
        positions: list[int] = []
        offset = 0
        while True:
            offset = tplg_data.find(needle, offset)
            if offset < 0:
                break
            positions.append(offset)
            offset += len(needle)
        if not positions:
            continue
        print(f"  {module.name:<8} {module.module_uuid} ({len(positions)} widget(s))")
        for pos in positions:
            before = [s for start, s in strings if start < pos][-2:]
            after = [s for start, s in strings if start >= pos][:2]
            print(f"    @{pos:#x}: {before} -> {after}")

    print("DMIC-related topology strings:")
    for offset, value in strings:
        if any(token in value.lower() for token in ("dmic", "pdm", "dai-copier.dmic")):
            print(f"  @{offset:#x}: {value}")

    token_names = {
        155: "DAI_INDEX",
        600: "DMIC_DRIVER_VERSION",
        601: "DMIC_CLK_MIN_HZ",
        602: "DMIC_CLK_MAX_HZ",
        603: "DMIC_DUTY_MIN_PERCENT",
        604: "DMIC_DUTY_MAX_PERCENT",
        605: "DMIC_NUM_PDM_ACTIVE",
        608: "DMIC_SAMPLE_RATE_HZ",
        609: "DMIC_FIFO_WORD_LENGTH_BITS",
        610: "DMIC_UNMUTE_RAMP_MS",
        700: "PDM_CTRL_ID",
        701: "PDM_MIC_A_ENABLE",
        702: "PDM_MIC_B_ENABLE",
        703: "PDM_POLARITY_A",
        704: "PDM_POLARITY_B",
        705: "PDM_CLK_EDGE",
        706: "PDM_SKEW",
    }
    print("Decoded DMIC/PDM vendor tuple arrays:")
    for array_offset in range(0, len(tplg_data) - 12, 2):
        size, array_type, count = struct.unpack_from("<III", tplg_data, array_offset)
        if count > 64 or size != 12 + 8 * count or array_type not in (1, 4, 5):
            continue
        pairs = [
            struct.unpack_from("<II", tplg_data, array_offset + 12 + 8 * i)
            for i in range(count)
        ]
        if not any(token in token_names for token, _ in pairs):
            continue
        print(f"  @{array_offset:#x} type={array_type}:")
        for token, value in pairs:
            if token in token_names:
                print(f"    {token_names[token]}={value}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
