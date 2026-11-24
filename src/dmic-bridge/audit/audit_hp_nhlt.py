#!/usr/bin/env python3
"""Print a compact, read-only summary of the HP NHLT DMIC endpoint."""

from __future__ import annotations

import argparse
import hashlib
import struct
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("table", type=Path)
    args = parser.parse_args()
    data = args.table.read_bytes()
    if len(data) < 37 or data[:4] != b"NHLT":
        parser.error("input is not a complete NHLT table")
    declared = struct.unpack_from("<I", data, 4)[0]
    count = data[36]
    if declared != len(data):
        parser.error(f"ACPI length mismatch: declared={declared}, actual={len(data)}")

    print(f"table={args.table.name} bytes={len(data)} sha256={hashlib.sha256(data).hexdigest()}")
    print(f"endpoint_count={count}")
    offset = 37
    for index in range(count):
        if offset + 23 > len(data):
            parser.error(f"truncated endpoint header at {offset:#x}")
        (length, link, instance, vendor, device, revision, subsystem,
         device_type, direction, virtual_bus, endpoint_config_size) = struct.unpack_from(
            "<IBBHHHIBBBI", data, offset
        )
        if length < 23 + endpoint_config_size or offset + length > len(data):
            parser.error(f"invalid endpoint length/config at {offset:#x}")
        config_start = offset + 23
        config = data[config_start:config_start + endpoint_config_size]
        print(
            f"EP{index} offset={offset:#x} length={length} link={link} instance={instance} "
            f"id={vendor:04x}:{device:04x} revision={revision:04x} subsystem={subsystem:08x} "
            f"device_type={device_type} direction={direction} virtual_bus={virtual_bus} "
            f"endpoint_config_bytes={endpoint_config_size}"
        )

        # NHLT endpoint configuration: config header is 4 bytes, then formats.
        fmt_offset = config_start + endpoint_config_size
        if fmt_offset >= offset + length:
            parser.error(f"missing format count at endpoint {index}")
        fmt_count = data[fmt_offset]
        cursor = fmt_offset + 1
        print(f"  format_count={fmt_count}")
        blobs: list[bytes] = []
        for fmt_index in range(fmt_count):
            if cursor + 44 > offset + length:
                parser.error(f"truncated format header at {cursor:#x}")
            (tag, channels, rate, avg_bytes, block_align, container_bits, cb_size,
             valid_bits, channel_mask, subformat) = struct.unpack_from(
                "<HHIIHHHHI16s", data, cursor
            )
            cursor += 40
            blob_size = struct.unpack_from("<I", data, cursor)[0]
            cursor += 4
            if cursor + blob_size > offset + length:
                parser.error(f"truncated format blob at {cursor:#x}")
            blob = data[cursor:cursor + blob_size]
            blobs.append(blob)
            cursor += blob_size
            print(
                f"  fmt{fmt_index}: tag={tag:#06x} channels={channels} rate={rate} "
                f"container_bits={container_bits} valid_bits={valid_bits} "
                f"block_align={block_align} channel_mask={channel_mask:#x} "
                f"blob_bytes={blob_size} blob_sha256={hashlib.sha256(blob).hexdigest()}"
            )
        if index == 0 and len(config) >= 4:
            print(
                f"  dmic_array: virtual_slot={config[0]} config_type={config[1]} "
                f"array_type={config[2]:#04x} microphone_count={config[3]} "
                f"vendor_extension_bytes={len(config) - 4}"
            )
        if index == 0 and len(blobs) == 2 and len(blobs[0]) == len(blobs[1]):
            differences = [i for i, (a, b) in enumerate(zip(*blobs)) if a != b]
            print(f"  fmt_blob_differing_offsets={differences}")
        offset += length
    if offset != len(data):
        print(f"trailing_table_bytes={len(data) - offset} (not part of counted endpoint records)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
