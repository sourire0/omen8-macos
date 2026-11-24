#!/usr/bin/env python3
"""Extract the matched public IPC4 DMIC-only topology candidate.

This is an offline binary slicer for one audited input topology. It preserves
selected records byte-for-byte, updates CoSA block and manifest counts, and
refuses unexpected DMIC graph shapes. The result is analysis material only;
it is not for installation or DSP loading.
"""

from __future__ import annotations

import argparse
import hashlib
import struct
from dataclasses import dataclass
from pathlib import Path


HEADER = struct.Struct("<9I")
MAGIC = 0x41536F43
ABI = 5
WIDGETS = {
    "dai-copier.DMIC.dmic01.capture",
    "gain.12.1",
    "eqiir.12.1",
    "module-copier.12.2",
    "pipeline.12",
}
ROUTES = {
    ("dai-copier.DMIC.dmic01.capture", "gain.12.1"),
    ("gain.12.1", "eqiir.12.1"),
    ("eqiir.12.1", "module-copier.12.2"),
}


@dataclass
class Block:
    offset: int
    header: tuple[int, ...]
    payload: bytes

    @property
    def kind(self) -> int:
        return self.header[3]

    @property
    def index(self) -> int:
        return self.header[7]


def cstr(data: bytes) -> str:
    return data.split(b"\0", 1)[0].decode("ascii", "replace")


def blocks(data: bytes) -> list[Block]:
    result: list[Block] = []
    off = 0
    while off < len(data):
        if off + HEADER.size > len(data):
            raise ValueError(f"truncated CoSA header at {off:#x}")
        h = HEADER.unpack_from(data, off)
        magic, abi, _version, _kind, hsize, _vendor, payload_size, _index, _count = h
        if magic != MAGIC or abi != ABI or hsize != HEADER.size:
            raise ValueError(f"unexpected CoSA header at {off:#x}: {h}")
        end = off + hsize + payload_size
        if end > len(data):
            raise ValueError(f"block at {off:#x} overruns source")
        result.append(Block(off, h, data[off + hsize : end]))
        off = end
    return result


def records(block: Block):
    """Yield exact object slices from supported ALSA topology block types."""
    p = block.payload
    kind = block.kind
    if kind == 4:  # snd_soc_tplg_dapm_graph_elem
        if len(p) != block.header[8] * 132:
            raise ValueError("unexpected graph block size")
        for off in range(0, len(p), 132):
            yield p[off : off + 132]
    elif kind == 5:  # widget + private tuples + embedded controls
        off = 0
        for _ in range(block.header[8]):
            if off + 132 > len(p):
                raise ValueError("truncated widget record")
            nk = struct.unpack_from("<I", p, off + 124)[0]
            private_size = struct.unpack_from("<I", p, off + 128)[0]
            q = off + 132 + private_size
            for _k in range(nk):
                if q + 8 > len(p):
                    raise ValueError("truncated widget control")
                control_type = struct.unpack_from("<I", p, q + 4)[0]
                base = {1: 360, 2: 240, 3: 1764}.get(control_type)
                if base is None or q + base > len(p):
                    raise ValueError(f"unsupported widget control type {control_type}")
                private = struct.unpack_from("<I", p, q + base - 4)[0]
                q += base + private
            if q > len(p):
                raise ValueError("widget record overruns block")
            yield p[off:q]
            off = q
        if off != len(p):
            raise ValueError("unparsed bytes in widget block")
    elif kind == 7:  # snd_soc_tplg_pcm + private data
        off = 0
        for _ in range(block.header[8]):
            if off + 912 > len(p):
                raise ValueError("truncated PCM record")
            size = struct.unpack_from("<I", p, off)[0]
            private = struct.unpack_from("<I", p, off + 908)[0]
            # The size field includes the private-size word already.
            end = off + size + private
            if end > len(p):
                raise ValueError("PCM record overruns block")
            yield p[off:end]
            off = end
        if off != len(p):
            raise ValueError("unparsed bytes in PCM block")
    elif kind in (6, 10):  # DAI_LINK / BACKEND_LINK + private data
        off = 0
        for _ in range(block.header[8]):
            if off + 1656 > len(p):
                raise ValueError("truncated DAI link record")
            size = struct.unpack_from("<I", p, off)[0]
            private = struct.unpack_from("<I", p, off + 1652)[0]
            # The size field includes the private-size word already.
            end = off + size + private
            if end > len(p):
                raise ValueError("DAI link record overruns block")
            yield p[off:end]
            off = end
        if off != len(p):
            raise ValueError("unparsed bytes in DAI link block")
    else:
        raise ValueError(f"unsupported record block type {kind}")


def update_block(block: Block, objects: list[bytes]) -> bytes:
    payload = b"".join(objects)
    h = list(block.header)
    h[6] = len(payload)
    h[8] = len(objects)
    return HEADER.pack(*h) + payload


def extract(source: Path, destination: Path) -> None:
    src = source.read_bytes()
    parsed = blocks(src)
    manifests = [b for b in parsed if b.kind == 8]
    if len(manifests) != 1 or manifests[0].header[8] != 1:
        raise ValueError("expected exactly one manifest record")
    selected: list[bytes] = []
    found_widgets: set[str] = set()
    found_routes: set[tuple[str, str]] = set()
    found_pcm = found_link = found_manifest = False

    for block in parsed:
        if block.kind == 8:
            obj = bytearray(block.payload)
            if len(obj) < 112:
                raise ValueError("truncated manifest")
            # control, widget, graph, PCM, link, physical DAI counts
            struct.pack_into("<6I", obj, 4, 0, len(WIDGETS), len(ROUTES), 1, 1, 0)
            selected.append(update_block(block, [bytes(obj)]))
            found_manifest = True
        elif block.kind == 5 and block.index == 12:
            keep = []
            for obj in records(block):
                name = cstr(obj[8:52])
                if name in WIDGETS:
                    found_widgets.add(name)
                    keep.append(obj)
            if keep:
                selected.append(update_block(block, keep))
        elif block.kind == 4:
            keep = []
            for obj in records(block):
                sink = cstr(obj[:44])
                source_name = cstr(obj[88:132])
                edge = (source_name, sink)
                if edge in ROUTES:
                    found_routes.add(edge)
                    keep.append(obj)
            if keep:
                selected.append(update_block(block, keep))
        elif block.kind == 7:
            keep = []
            for obj in records(block):
                name = cstr(obj[4:48])
                if name == "DMIC Raw":
                    pcm_id, dai_id, playback, capture = struct.unpack_from("<4I", obj, 92)
                    if (pcm_id, dai_id, playback, capture) != (6, 6, 0, 1):
                        raise ValueError("DMIC Raw PCM references or direction changed")
                    found_pcm = True
                    keep.append(obj)
            if keep:
                selected.append(update_block(block, keep))
        elif block.kind == 10:
            keep = []
            for obj in records(block):
                link_id = struct.unpack_from("<I", obj, 4)[0]
                name = cstr(obj[8:52])
                if link_id == 6 and name == "dmic01":
                    found_link = True
                    keep.append(obj)
            if keep:
                selected.append(update_block(block, keep))
        # Other blocks in the audited source are unrelated HDA/HDMI records.

    if not found_manifest or found_widgets != WIDGETS or found_routes != ROUTES:
        raise ValueError(f"DMIC graph mismatch: widgets={found_widgets}, routes={found_routes}")
    if not found_pcm or not found_link:
        raise ValueError("missing DMIC Raw PCM or dmic01 backend link")

    # Refuse to silently omit any block type beyond the audited public pair.
    allowed = {4, 5, 7, 8, 10}
    unexpected = sorted({b.kind for b in parsed} - allowed)
    if unexpected:
        raise ValueError(f"source topology gained unaudited block types: {unexpected}")

    out = b"".join(selected)
    destination.write_bytes(out)
    print(f"candidate={destination}")
    print(f"bytes={len(out)} sha256={hashlib.sha256(out).hexdigest()}")
    print(f"widgets={len(found_widgets)} routes={len(found_routes)} PCM=DMIC Raw link=6/dmic01")
    print("warning=analysis candidate only; do not install or load into the DSP")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("destination", type=Path)
    args = parser.parse_args()
    extract(args.source, args.destination)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
