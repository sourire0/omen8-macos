#!/usr/bin/env python3
"""Independently validate a reduced IPC4 DMIC topology against its source."""

from __future__ import annotations

import argparse
import hashlib
import importlib.util
import struct
import sys
import uuid
from pathlib import Path


HDR = struct.Struct("<9I")
MAGIC = 0x41536F43
EXPECTED_WIDGETS = {
    "dai-copier.DMIC.dmic01.capture",
    "gain.12.1",
    "eqiir.12.1",
    "module-copier.12.2",
    "pipeline.12",
}
EXPECTED_ROUTES = {
    ("dai-copier.DMIC.dmic01.capture", "gain.12.1"),
    ("gain.12.1", "eqiir.12.1"),
    ("eqiir.12.1", "module-copier.12.2"),
}


def need(ok: bool, message: str) -> None:
    if not ok:
        raise ValueError(message)


def cstr(data: bytes) -> str:
    return data.split(b"\0", 1)[0].decode("ascii", "replace")


def parse_blocks(data: bytes):
    result = []
    offset = 0
    while offset < len(data):
        need(offset + HDR.size <= len(data), f"truncated block header at {offset:#x}")
        header = HDR.unpack_from(data, offset)
        need(header[0] == MAGIC and header[1] == 5 and header[4] == HDR.size,
             f"invalid CoSA header at {offset:#x}")
        end = offset + header[4] + header[6]
        need(end <= len(data), f"block at {offset:#x} overruns file")
        payload = data[offset + HDR.size : end]
        result.append((header, payload))
        offset = end
    need(offset == len(data), "trailing bytes after last block")
    return result


def records(header, payload: bytes):
    kind, count = header[3], header[8]
    if kind == 4:  # graph elements are fixed-size
        need(len(payload) == count * 132, "graph block length/count mismatch")
        return [payload[i : i + 132] for i in range(0, len(payload), 132)]
    out = []
    offset = 0
    if kind == 5:
        for _ in range(count):
            need(offset + 132 <= len(payload), "truncated widget base record")
            controls, private_size = struct.unpack_from("<II", payload, offset + 124)
            end = offset + 132 + private_size
            for _ in range(controls):
                need(end + 8 <= len(payload), "truncated embedded control")
                typ = struct.unpack_from("<I", payload, end + 4)[0]
                size = {1: 360, 2: 240, 3: 1764}.get(typ)
                need(size is not None and end + size <= len(payload), "unknown control record")
                end += size + struct.unpack_from("<I", payload, end + size - 4)[0]
            out.append(payload[offset:end])
            offset = end
    elif kind == 7:
        for _ in range(count):
            need(offset + 912 <= len(payload), "truncated PCM")
            size = struct.unpack_from("<I", payload, offset)[0]
            private_size = struct.unpack_from("<I", payload, offset + 908)[0]
            end = offset + size + private_size
            need(end <= len(payload), "PCM record overruns block")
            out.append(payload[offset:end])
            offset = end
    elif kind in (6, 10):
        for _ in range(count):
            need(offset + 1656 <= len(payload), "truncated link")
            size = struct.unpack_from("<I", payload, offset)[0]
            private_size = struct.unpack_from("<I", payload, offset + 1652)[0]
            end = offset + size + private_size
            need(end <= len(payload), "link record overruns block")
            out.append(payload[offset:end])
            offset = end
    elif kind == 8:
        need(count == 1, "manifest block must contain one record")
        return [payload]
    else:
        raise ValueError(f"unhandled block type {kind}")
    need(offset == len(payload), f"unparsed bytes in block type {kind}")
    return out


def iter_records(parsed):
    for header, payload in parsed:
        for record in records(header, payload):
            yield header[3], header[7], record


def parse_tuples(data: bytes):
    values = []
    offset = 0
    while offset < len(data):
        need(offset + 12 <= len(data), "truncated tuple array")
        size, typ, count = struct.unpack_from("<III", data, offset)
        need(size >= 12 and offset + size <= len(data), "invalid tuple array size")
        pos = offset + 12
        for _ in range(count):
            need(pos + 4 <= offset + size, "truncated tuple")
            token = struct.unpack_from("<I", data, pos)[0]
            if typ == 0:
                need(pos + 20 <= offset + size, "truncated UUID tuple")
                value = str(uuid.UUID(bytes_le=data[pos + 4 : pos + 20]))
                pos += 20
            elif typ == 1:
                need(pos + 48 <= offset + size, "truncated string tuple")
                value = cstr(data[pos + 4 : pos + 48])
                pos += 48
            else:
                need(pos + 8 <= offset + size, "truncated value tuple")
                value = struct.unpack_from("<I", data, pos + 4)[0]
                pos += 8
            values.append((typ, token, value))
        need(pos == offset + size, "tuple count does not fill array")
        offset += size
    return values


def load_firmware_parser(script_dir: Path):
    path = script_dir / "audit_oem_ipc4.py"
    spec = importlib.util.spec_from_file_location("audit_oem_ipc4", path)
    need(spec is not None and spec.loader is not None, "cannot load firmware parser")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def validate(candidate: Path, source: Path, firmware_path: Path, parser_dir: Path) -> None:
    candidate_data, source_data = candidate.read_bytes(), source.read_bytes()
    cand, src = parse_blocks(candidate_data), parse_blocks(source_data)
    cand_records, src_records = list(iter_records(cand)), list(iter_records(src))

    # Container structure and counts.
    allowed = {4, 5, 7, 8, 10}
    need({h[3] for h, _ in cand} <= allowed, "candidate contains unapproved block type")
    manifest = next(r for t, _i, r in cand_records if t == 8)
    declared = struct.unpack_from("<6I", manifest, 4)
    actual = (
        0,
        sum(t == 5 for t, _i, _r in cand_records),
        sum(t == 4 for t, _i, _r in cand_records),
        sum(t == 7 for t, _i, _r in cand_records),
        sum(t in (6, 10) for t, _i, _r in cand_records),
        sum(t == 12 for t, _i, _r in cand_records),
    )
    need(declared == actual == (0, 5, 3, 1, 1, 0),
         f"manifest counts {declared} differ from actual {actual}")

    # Each selected non-manifest object must be byte-exact from source.
    source_objects = {(t, i, rec) for t, i, rec in src_records}
    for t, i, rec in cand_records:
        if t != 8:
            need((t, i, rec) in source_objects,
                 f"type {t} index {i} record was modified or not in source")

    widgets = [r for t, _i, r in cand_records if t == 5]
    widget_by_name = {cstr(r[8:52]): r for r in widgets}
    need(set(widget_by_name) == EXPECTED_WIDGETS, "unexpected DMIC widget selection")
    route_pairs = {(cstr(r[88:132]), cstr(r[:44]))
                   for t, _i, r in cand_records if t == 4}
    need(route_pairs == EXPECTED_ROUTES, "DMIC graph route set mismatch")

    # DAI copier identity and candidate DMIC/PDM backend configuration.
    dai = widget_by_name["dai-copier.DMIC.dmic01.capture"]
    dai_tuples = parse_tuples(dai[132 : 132 + struct.unpack_from("<I", dai, 128)[0]])
    need((1, 154, "DMIC") in dai_tuples and (4, 155, 0) in dai_tuples,
         "DMIC DAI type/index mismatch")

    pcm = next(r for t, _i, r in cand_records if t == 7)
    need(cstr(pcm[4:48]) == "DMIC Raw", "wrong PCM retained")
    need(struct.unpack_from("<4I", pcm, 92) == (6, 6, 0, 1),
         "DMIC Raw PCM id/direction mismatch")
    link = next(r for t, _i, r in cand_records if t == 10)
    need(struct.unpack_from("<I", link, 4)[0] == 6 and cstr(link[8:52]) == "dmic01",
         "dmic01 backend link mismatch")
    link_private_size = struct.unpack_from("<I", link, 1652)[0]
    link_tuples = parse_tuples(link[1656 : 1656 + link_private_size])
    tuple_words = {(token, value) for typ, token, value in link_tuples}
    for expected in ((600, 1), (601, 500000), (602, 4800000), (603, 40),
                     (604, 60), (605, 2), (608, 48000), (609, 32), (610, 200),
                     (155, 0), (700, 0), (701, 1), (702, 1), (700, 1),
                     (701, 0), (702, 0)):
        need(expected in tuple_words, f"missing backend DMIC tuple {expected}")

    # Preserve the opaque but required IIR payload and validate its ABI envelope.
    eq = widget_by_name["eqiir.12.1"]
    nk, private_size = struct.unpack_from("<II", eq, 124)
    pos = 132 + private_size
    eq_payload = None
    for _ in range(nk):
        typ = struct.unpack_from("<I", eq, pos + 4)[0]
        name = cstr(eq[pos + 8 : pos + 52])
        base = {1: 360, 2: 240, 3: 1764}[typ]
        n = struct.unpack_from("<I", eq, pos + base - 4)[0]
        if name == "DMIC0 Capture IIR Eq":
            eq_payload = eq[pos + base : pos + base + n]
        pos += base + n
    need(eq_payload is not None and len(eq_payload) == 120, "missing IIR ABI payload")
    magic, param_type, size, abi = struct.unpack_from("<4I", eq_payload)
    need((magic, param_type, size, abi) == (0x34464F53, 0, 88, 0x0301A000),
         "IIR ABI envelope mismatch")

    # Check DAI copier, EQIIR and gain UUIDs against matched signed firmware table.
    fw_parser = load_firmware_parser(parser_dir)
    fw = fw_parser.parse_firmware(firmware_path.name, firmware_path.read_bytes())
    fw_uuids = {m.module_uuid for m in fw.modules}
    for name in ("dai-copier.DMIC.dmic01.capture", "eqiir.12.1", "gain.12.1"):
        rec = widget_by_name[name]
        _nk, private_size = struct.unpack_from("<II", rec, 124)
        pos, end = 132, 132 + private_size
        found = set()
        while pos + 12 <= end:
            size, typ, count = struct.unpack_from("<III", rec, pos)
            item = pos + 12
            for _ in range(count):
                token = struct.unpack_from("<I", rec, item)[0]
                if typ == 0:
                    found.add(str(uuid.UUID(bytes_le=rec[item + 4 : item + 20])))
                    item += 20
                elif typ == 1:
                    item += 48
                else:
                    item += 8
            need(item == pos + size, f"malformed tuple list in {name}")
            pos += size
        need(found & fw_uuids, f"no firmware module UUID match for {name}")

    print(f"PASS: {candidate}")
    print(f"bytes={len(candidate_data)} sha256={hashlib.sha256(candidate_data).hexdigest()}")
    print("CoSA block boundaries and manifest counts: pass")
    print("source-record byte preservation, DMIC graph/PCM/link references: pass")
    print("NHLT DMIC tuples, EQIIR ABI envelope and firmware UUID matches: pass")
    print("scope=static file validation only; no Linux loader, firmware or hardware execution")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("candidate", type=Path)
    parser.add_argument("source", type=Path)
    parser.add_argument("firmware", type=Path)
    parser.add_argument("--parser-dir", type=Path,
                         default=Path(__file__).resolve().parent)
    args = parser.parse_args()
    validate(args.candidate, args.source, args.firmware, args.parser_dir)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError, struct.error) as exc:
        print(f"FAIL: {exc}", file=sys.stderr)
        raise SystemExit(1)
