#!/usr/bin/env python3
"""Read-only parser for SOF IPC4 firmware images and HP's OEM driver ZIP.

This tool parses package bytes only. It does not extract files, modify the ZIP,
load a kext, access hardware, or send IPC messages.
"""

from __future__ import annotations

import argparse
import hashlib
import struct
import sys
import uuid
import zipfile
import xml.etree.ElementTree as ET
from dataclasses import dataclass
from pathlib import PureWindowsPath


AE1 = b"$AE1"
AM1 = b"$AM1"
FW_HEADER_OFFSET = 0x2000
EXT_HEADER_SIZE = 16
FW_HEADER = struct.Struct("<II8sIIIHHHHIIII")
MODULE_ENTRY_SIZE = 116
MODULE_CONFIG = struct.Struct("<IIIIIIIIIII")


class AuditError(ValueError):
    pass


@dataclass(frozen=True)
class Module:
    index: int
    module_id: int
    name: str
    module_uuid: str
    module_type: int
    cfg_offset: int
    cfg_count: int


@dataclass(frozen=True)
class Firmware:
    name: str
    size: int
    sha256: str
    manifest_length: int
    header_offset: int
    manifest_version: tuple[int, int]
    image_name: str
    version: tuple[int, int, int, int]
    module_metadata_sha256: str
    modules: tuple[Module, ...]


def parse_firmware(name: str, blob: bytes) -> Firmware:
    if len(blob) < EXT_HEADER_SIZE:
        raise AuditError(f"{name}: truncated extended manifest header")
    magic, manifest_length, ext_major, ext_minor, ext_modules = struct.unpack_from(
        "<4sIHHI", blob, 0
    )
    if magic != AE1:
        raise AuditError(f"{name}: expected $AE1, got {magic!r}")
    if manifest_length < EXT_HEADER_SIZE or manifest_length > len(blob):
        raise AuditError(f"{name}: invalid manifest length 0x{manifest_length:x}")

    fw_offset = manifest_length + FW_HEADER_OFFSET
    if fw_offset + FW_HEADER.size > len(blob):
        raise AuditError(f"{name}: firmware header falls outside image")
    (
        fw_magic,
        fw_header_length,
        fw_name_raw,
        _preload_pages,
        _image_flags,
        _feature_mask,
        major,
        minor,
        hotfix,
        build,
        fw_modules,
        _hw_buf_base,
        _hw_buf_length,
        _load_offset,
    ) = FW_HEADER.unpack_from(blob, fw_offset)
    if struct.pack("<I", fw_magic) != AM1:
        actual = blob[fw_offset : fw_offset + 4]
        raise AuditError(
            f"{name}: expected $AM1 at ext_len + 0x2000, got {actual!r}"
        )
    if fw_header_length < FW_HEADER.size:
        raise AuditError(f"{name}: invalid firmware header length {fw_header_length}")
    if fw_offset + fw_header_length > len(blob):
        raise AuditError(f"{name}: firmware header length exceeds image")
    if fw_modules != ext_modules:
        raise AuditError(
            f"{name}: manifest/header module count mismatch "
            f"({ext_modules} vs {fw_modules})"
        )
    if fw_modules > 4096:
        raise AuditError(f"{name}: unreasonable module count {fw_modules}")

    modules_start = fw_offset + fw_header_length
    modules_end = modules_start + fw_modules * MODULE_ENTRY_SIZE
    if modules_end > len(blob):
        raise AuditError(f"{name}: module table falls outside image")

    modules: list[Module] = []
    max_cfg_end = 0
    for index in range(fw_modules):
        off = modules_start + index * MODULE_ENTRY_SIZE
        module_id = struct.unpack_from("<I", blob, off)[0]
        raw_name = blob[off + 4 : off + 12]
        module_name = raw_name.split(b"\0", 1)[0].decode("ascii", "replace")
        raw_uuid = blob[off + 12 : off + 28]
        module_uuid = str(uuid.UUID(bytes_le=raw_uuid))
        module_type = struct.unpack_from("<I", blob, off + 28)[0]
        cfg_offset, cfg_count = struct.unpack_from("<HH", blob, off + 68)
        max_cfg_end = max(max_cfg_end, cfg_offset + cfg_count)
        modules.append(
            Module(
                index,
                module_id,
                module_name,
                module_uuid,
                module_type,
                cfg_offset,
                cfg_count,
            )
        )

    configs_end = modules_end + max_cfg_end * MODULE_CONFIG.size
    if configs_end > len(blob):
        raise AuditError(f"{name}: module configuration table falls outside image")

    return Firmware(
        name=name,
        size=len(blob),
        sha256=hashlib.sha256(blob).hexdigest(),
        manifest_length=manifest_length,
        header_offset=fw_offset,
        manifest_version=(ext_major, ext_minor),
        image_name=fw_name_raw.split(b"\0", 1)[0].decode("ascii", "replace"),
        version=(major, minor, hotfix, build),
        module_metadata_sha256=hashlib.sha256(blob[modules_start:configs_end]).hexdigest(),
        modules=tuple(modules),
    )


def member_ending(zip_file: zipfile.ZipFile, suffix: str) -> str:
    matches = [
        name
        for name in zip_file.namelist()
        if PureWindowsPath(name).name.lower() == suffix.lower()
    ]
    if len(matches) != 1:
        raise AuditError(f"ZIP member {suffix!r}: expected one match, found {len(matches)}")
    return matches[0]


def read_firmware(zip_file: zipfile.ZipFile, suffix: str) -> Firmware:
    member = member_ending(zip_file, suffix)
    return parse_firmware(member, zip_file.read(member))


def print_firmware(fw: Firmware) -> None:
    print(
        f"{fw.name}\n"
        f"  bytes={fw.size} sha256={fw.sha256}\n"
        f"  $AE1 len=0x{fw.manifest_length:x}; $AM1 offset=0x{fw.header_offset:x}; "
        f"manifest={fw.manifest_version[0]}.{fw.manifest_version[1]} "
        f"image={fw.image_name!r} version={'.'.join(map(str, fw.version))}; "
        f"module-table+configs-sha256={fw.module_metadata_sha256}; "
        f"modules={len(fw.modules)}"
    )
    for module in fw.modules:
        print(
            f"  [{module.index:02}] id=0x{module.module_id:08x} "
            f"name={module.name:<8} type={module.module_type} "
            f"uuid={module.module_uuid} cfg={module.cfg_offset}+{module.cfg_count}"
        )


def print_dmic_oed_graph(xml_data: bytes) -> None:
    root = ET.fromstring(xml_data)
    paths: dict[str, tuple[str, list[str]]] = {}
    for path in root.findall(".//Path"):
        path_id = path.attrib.get("Id", "")
        path_name = path.attrib.get("Name", path_id)
        modules = [node.attrib.get("ModuleType", "") for node in path.findall("./Node")]
        paths[path_id] = (path_name, modules)

    print("OEM OED DMIC streams:")
    for stream in root.findall(".//Stream"):
        if "dmic" not in stream.attrib.get("Name", "").lower():
            continue
        stream_type = stream.find("./StreamType")
        st = stream_type.attrib if stream_type is not None else {}
        print(
            f"  {stream.attrib.get('Name', '')}: link={st.get('Link')} "
            f"type={st.get('Type')} pipe={st.get('Pipe')}"
        )
        seen: set[str] = set()
        for item in stream.findall("./PathItem"):
            for edge in ("SrcPath", "DstPath"):
                node = item.find(f"./{edge}")
                if node is None:
                    continue
                path_id = node.attrib.get("Id", "")
                if path_id in seen:
                    continue
                seen.add(path_id)
                path_name, modules = paths.get(path_id, (path_id, []))
                print(f"    {path_name}: {' -> '.join(modules)}")


def print_oed_uuid_evidence(modules: tuple[Module, ...], files: list[tuple[str, bytes]]) -> None:
    print("UUID byte matches in OEM OED artifacts (GUID little-endian encoding):")
    seen: set[str] = set()
    for module in modules:
        if module.module_uuid in seen or module.module_uuid == str(uuid.UUID(int=0)):
            continue
        seen.add(module.module_uuid)
        needle = uuid.UUID(module.module_uuid).bytes_le
        hits = [f"{name}@0x{data.find(needle):x}" for name, data in files if data.find(needle) >= 0]
        if hits:
            print(f"  {module.name:<8} {module.module_uuid}: {', '.join(hits)}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("driver_zip", help="HP driver export ZIP; input is read-only")
    args = parser.parse_args()
    try:
        with zipfile.ZipFile(args.driver_zip, "r") as archive:
            firmware_names = (
                "dsp_fw_adl_release.bin",
                "dsp_fw_adl-n_release.bin",
                "dsp_lib_whm_adl_release.bin",
                "dsp_lib_whm_adl-n_release.bin",
                "dsp_lib_intel_wov_adl_release.bin",
                "dsp_lib_intel_wov_adl-n_release.bin",
            )
            parsed: list[Firmware] = []
            for suffix in firmware_names:
                fw = read_firmware(archive, suffix)
                parsed.append(fw)
                print_firmware(fw)
                print()
            xml_name = member_ending(archive, "IntcOEDTopology.xml")
            xml_data = archive.read(xml_name)
            print_dmic_oed_graph(xml_data)
            oed_name = member_ending(archive, "IntcOEDTopology.oec")
            sys_name = member_ending(archive, "IntcOED.sys")
            modules = tuple(module for fw in parsed for module in fw.modules)
            print_oed_uuid_evidence(
                modules,
                [
                    (xml_name, xml_data),
                    (oed_name, archive.read(oed_name)),
                    (sys_name, archive.read(sys_name)),
                ],
            )
    except (OSError, zipfile.BadZipFile, KeyError, AuditError, ET.ParseError) as exc:
        print(f"audit error: {exc}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
