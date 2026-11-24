#!/usr/bin/env python3
"""Emit an offline, machine-readable plan from the audited IPC4 DMIC graph."""

from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import struct
import sys
from pathlib import Path


INPUT_FORMAT_TOKENS = {
    1900: "sampling_frequency",
    1901: "bit_depth",
    1904: "channel_map",
    1905: "channel_config",
    1906: "interleaving_style",
    1907: "format_config",
    1909: "pin_index",
    1970: "buffer_size",
}
OUTPUT_FORMAT_TOKENS = {
    1930: "sampling_frequency",
    1931: "bit_depth",
    1934: "channel_map",
    1935: "channel_config",
    1936: "interleaving_style",
    1937: "format_config",
    1939: "pin_index",
    1971: "buffer_size",
}
STREAM_FORMAT = {
    "sampling_frequency": 48000,
    "channels": 2,
    "valid_bit_depth": 32,
    "sample_type": 1,
}


def load_module(name: str, path: Path):
    spec = importlib.util.spec_from_file_location(name, path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load parser: {path}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def parse_uuid_tuples(raw: bytes, parser) -> list[str]:
    return [value for kind, _token, value in parser.parse_tuples(raw) if kind == 0]


def parse_tuple_arrays(raw: bytes, parser) -> list[dict]:
    arrays = []
    offset = 0
    while offset < len(raw):
        if offset + 12 > len(raw):
            raise ValueError("truncated private tuple-array header")
        size, tuple_type, count = struct.unpack_from("<III", raw, offset)
        if size < 12 or offset + size > len(raw):
            raise ValueError("invalid private tuple-array size")
        array_bytes = raw[offset : offset + size]
        values = parser.parse_tuples(array_bytes)
        if len(values) != count or any(kind != tuple_type for kind, _, _ in values):
            raise ValueError("private tuple-array metadata does not match its entries")
        arrays.append({
            "tuple_type": tuple_type,
            "count": count,
            "values": [{"type": kind, "token": token, "value": value}
                       for kind, token, value in values],
        })
        offset += size
    return arrays


def parse_audio_formats(tuples: list[tuple], count_token: int,
                        token_fields: dict[int, str]) -> list[dict]:
    """Mirror sof_update_ipc_object() token-instance semantics for IPC4 formats."""
    count_values = [value for _kind, token, value in tuples if token == count_token]
    if not count_values:
        return []
    count = int(count_values[0])
    formats = [dict() for _ in range(count)]
    for token, field in token_fields.items():
        values = [int(value) for _kind, item_token, value in tuples
                  if item_token == token]
        for index, value in enumerate(values[:count]):
            formats[index][field] = value
    for item in formats:
        fmt = item.get("format_config", 0)
        item["channels"] = fmt & 0xff
        item["valid_bit_depth"] = (fmt >> 8) & 0xff
        item["sample_type"] = (fmt >> 16) & 0xff
    return formats


def parse_firmware_module_configs(blob: bytes, firmware, parser) -> dict[str, list[dict]]:
    fw_offset = firmware.manifest_length + parser.FW_HEADER_OFFSET
    fw_header_length = struct.unpack_from("<I", blob, fw_offset + 4)[0]
    modules_start = fw_offset + fw_header_length
    configs_start = modules_start + len(firmware.modules) * parser.MODULE_ENTRY_SIZE
    configs: dict[str, list[dict]] = {}
    for module in firmware.modules:
        entries = []
        for index in range(module.cfg_offset, module.cfg_offset + module.cfg_count):
            offset = configs_start + index * parser.MODULE_CONFIG.size
            if offset + parser.MODULE_CONFIG.size > len(blob):
                raise ValueError(f"manifest config for {module.name} exceeds firmware image")
            words = parser.MODULE_CONFIG.unpack_from(blob, offset)
            entries.append({
                "is_bytes": words[4], "cycles_per_second": words[5],
                "ibs": words[6], "obs": words[7],
                "module_flags": words[8], "cpc": words[9], "obls": words[10],
            })
        configs[module.module_uuid] = entries
    return configs


def select_format(formats: list[dict], contract: dict, widget: str,
                  direction: str) -> tuple[int, dict]:
    for index, item in enumerate(formats):
        if all(item.get(key) == value for key, value in contract.items()):
            return index, item
    raise ValueError(f"{widget}: no {direction} format matches selected stream contract")


def module_resource_pages(module_type: int, input_buffer_bytes: int,
                          is_pages: int) -> dict:
    # Constants and equations match sof_ipc4_update_resource_usage() in SOF.
    page_size, queue_count, queue_bytes = 4096, 8, 128
    pipeline_bytes, instance_bytes = 448, 12
    ll_task_bytes, dp_task_bytes = 72, 104
    ll_flag, dp_flag = 1 << 5, 1 << 6
    align64 = lambda value: (value + 63) & ~63
    if module_type & ll_flag:
        task_bytes = (pipeline_bytes + instance_bytes + is_pages + align64(ll_task_bytes)
                      + queue_count * instance_bytes + 12)
        domain = "low_latency"
    elif module_type & dp_flag:
        task_bytes = (pipeline_bytes + instance_bytes + is_pages + align64(dp_task_bytes)
                      + 12 + 8)
        domain = "data_processing"
    else:
        raise ValueError(f"unsupported IPC4 module scheduling type {module_type:#x}")
    queue_memory = queue_count * (queue_bytes + align64(input_buffer_bytes))
    total_bytes = task_bytes + queue_memory
    return {
        "domain": domain,
        "task_bytes": task_bytes,
        "queue_bytes": queue_memory,
        "total_bytes_before_page_roundup": total_bytes,
        "pages": (total_bytes + page_size - 1) // page_size,
    }


def extract_hp_dmic_nhlt(path: Path) -> dict:
    data = path.read_bytes()
    if len(data) < 37 or data[:4] != b"NHLT" or struct.unpack_from("<I", data, 4)[0] != len(data):
        raise ValueError("invalid NHLT table header or declared length")
    endpoints = data[36]
    offset = 37
    for endpoint_index in range(endpoints):
        if offset + 23 > len(data):
            raise ValueError("truncated NHLT endpoint header")
        (length, link, instance, vendor, device, revision, subsystem,
         device_type, direction, virtual_bus, config_size) = struct.unpack_from(
            "<IBBHHHIBBBI", data, offset)
        end = offset + length
        if length < 23 + config_size or end > len(data):
            raise ValueError("invalid NHLT endpoint size")
        cursor = offset + 23 + config_size
        if cursor >= end:
            raise ValueError("NHLT endpoint has no formats")
        count = data[cursor]
        cursor += 1
        for fmt_index in range(count):
            if cursor + 44 > end:
                raise ValueError("truncated NHLT wave format")
            (tag, channels, rate, _avg, _align, container_bits, _cb,
             valid_bits, _mask, _subformat) = struct.unpack_from(
                "<HHIIHHHHI16s", data, cursor)
            cursor += 40
            blob_size = struct.unpack_from("<I", data, cursor)[0]
            cursor += 4
            if cursor + blob_size > end:
                raise ValueError("truncated NHLT format blob")
            blob = data[cursor:cursor + blob_size]
            cursor += blob_size
            if (vendor, device, link, direction, channels, rate, container_bits, valid_bits) == (
                    0x8086, 0xAE20, 2, 1, 2, 48000, 32, 32):
                return {
                    "table": path.name,
                    "table_sha256": hashlib.sha256(data).hexdigest(),
                    "endpoint_index": endpoint_index,
                    "format_index": fmt_index,
                    "vendor_device": "8086:AE20",
                    "link_type": link,
                    "direction": direction,
                    "channels": channels,
                    "rate": rate,
                    "container_bits": container_bits,
                    "valid_bits": valid_bits,
                    "blob_bytes": len(blob),
                    "blob_sha256": hashlib.sha256(blob).hexdigest(),
                    "blob_hex": blob.hex(),
                    "note": "raw endpoint format blob; intended for DAI copier NHLT config after struct layout review",
                }
        offset = end
    raise ValueError("no 48 kHz 2-channel 32-bit Intel AE20 DMIC capture blob found")


def main() -> int:
    cli = argparse.ArgumentParser(description=__doc__)
    cli.add_argument("candidate", type=Path)
    cli.add_argument("source", type=Path)
    cli.add_argument("firmware", type=Path)
    cli.add_argument("output", type=Path)
    cli.add_argument("--audit-dir", type=Path, required=True)
    cli.add_argument("--nhlt", type=Path)
    args = cli.parse_args()

    audit = load_module("validate_ipc4_dmic_candidate", args.audit_dir / "validate_ipc4_dmic_candidate.py")
    fw_audit = load_module("audit_oem_ipc4", args.audit_dir / "audit_oem_ipc4.py")
    audit.validate(args.candidate, args.source, args.firmware, args.audit_dir)

    topology = args.candidate.read_bytes()
    firmware_bytes = args.firmware.read_bytes()
    fw = fw_audit.parse_firmware(args.firmware.name, firmware_bytes)
    module_by_uuid = {module.module_uuid: module for module in fw.modules}
    module_configs = parse_firmware_module_configs(firmware_bytes, fw, fw_audit)
    parsed = audit.parse_blocks(topology)
    records = list(audit.iter_records(parsed))

    widgets = {}
    startup_control_updates = []
    for kind, index, record in records:
        if kind != 5:
            continue
        name = audit.cstr(record[8:52])
        control_count, private_size = struct.unpack_from("<II", record, 124)
        private = record[132 : 132 + private_size]
        audio_tuples = audit.parse_tuples(private)
        input_formats = parse_audio_formats(audio_tuples, 415, INPUT_FORMAT_TOKENS)
        output_formats = parse_audio_formats(audio_tuples, 416, OUTPUT_FORMAT_TOKENS)
        uuids = parse_uuid_tuples(private, audit)
        modules = []
        for value in uuids:
            module = module_by_uuid.get(value)
            if module is not None:
                modules.append({"uuid": value, "manifest_index": module.index,
                                "manifest_name": module.name})
        controls = []
        # ASoC assigns kcontrol indexes independently for mixer, enum, and
        # bytes controls within each widget. SOF IPC4 copies that index into
        # its per-control data header (see topology.c control_load and
        # ipc4-topology.c control_data->index).
        control_indexes = {1: 0, 2: 0, 3: 0}
        pos = 132 + private_size
        for _ in range(control_count):
            control_type = struct.unpack_from("<I", record, pos + 4)[0]
            control_name = audit.cstr(record[pos + 8 : pos + 52])
            base_size = {1: 360, 2: 240, 3: 1764}.get(control_type)
            if base_size is None:
                raise ValueError(f"unexpected control type {control_type} in {name}")
            payload_size = struct.unpack_from("<I", record, pos + base_size - 4)[0]
            control_data = record[pos : pos + base_size + payload_size]
            control_plan = {
                "name": control_name,
                "type_code": control_type,
                "control_index_within_type": control_indexes[control_type],
                "control_index_basis": "ASoC per-widget index counter for this control type",
                "bytes": len(control_data),
                "sha256": hashlib.sha256(control_data).hexdigest(),
                "record_hex": control_data.hex(),
            }
            control_indexes[control_type] += 1
            if control_type == 1:
                # Offsets follow the fixed UAPI snd_soc_tplg_mixer_control ABI.
                control_max = struct.unpack_from("<I", control_data, 212)[0]
                num_channels = struct.unpack_from("<I", control_data, 224)[0]
                if not 1 <= num_channels <= 8:
                    raise ValueError(f"invalid channel count in mixer control {control_name}")
                control_plan["mixer_metadata"] = {
                    "max": control_max,
                    "num_channels": num_channels,
                    "num_channels_source_offset": 224,
                }
                if control_name == "Dmic0 Capture Switch":
                    if control_plan["control_index_within_type"] != 1:
                        raise ValueError("expected switch mixer control index 1 after volume index 0")
                    param_id = 200
                    channel_values = b"".join(
                        struct.pack("<2I", channel, 1)
                        for channel in range(num_channels))
                    payload = struct.pack("<HH4I", 0, num_channels, 0, 0, 0, 0) + channel_values
                    primary = ((1 << 30) | (4 << 24) |
                               (modules[0]["manifest_index"] if len(modules) == 1 else 0))
                    extension = (1 << 29) | (1 << 28) | (param_id << 20) | len(payload)
                    update = {
                        "control_name": control_name,
                        "module_uuid": modules[0]["uuid"] if len(modules) == 1 else None,
                        "module_id": modules[0]["manifest_index"] if len(modules) == 1 else None,
                        "module_instance": "runtime allocation",
                        "control_id": control_plan["control_index_within_type"],
                        "control_id_status": "derived from per-widget mixer index ordering",
                        "param_id": param_id,
                        "default_state_from_sof_control_setup": "off",
                        "requested_state_before_capture": "on",
                        "channel_count": num_channels,
                        "message_primary_template": primary,
                        "message_extension": extension,
                        "payload_bytes": len(payload),
                        "payload_hex_control_id_zero_template": payload.hex(),
                        "payload_sha256_control_id_zero_template": hashlib.sha256(payload).hexdigest(),
                        "ordering": "after widget control defaults and before pipeline capture start",
                    }
                    control_plan["capture_enable_update"] = update
                    startup_control_updates.append(update)
            if control_type == 2:
                if control_name == "DMIC0 Capture IIR Eq" and control_plan["control_index_within_type"] != 0:
                    raise ValueError("expected the EQIIR bytes control to have bytes index 0")
                abi = control_data[base_size:]
                if len(abi) < 32:
                    raise ValueError(f"short SOF ABI header in bytes control {control_name}")
                magic, parameter_id, data_size, abi_version = struct.unpack_from("<IIII", abi)
                if (magic != 0x34464F53 or parameter_id > 0xFF or
                        len(abi) != 32 + data_size):
                    raise ValueError(f"invalid SOF IPC4 ABI envelope in {control_name}")
                ipc_payload = abi[32:]
                # IPC4 module large-config transfers carry parameter bytes
                # directly. The generic control-message header belongs to
                # ALSA channel controls; prepending it to EQIIR's algorithm
                # blob makes the firmware parse the reserved header as the
                # EQIIR response/channel counts (both zero).
                control_payload = ipc_payload
                control_plan["ipc4_large_config_set"] = {
                    "module_uuid": uuids[0] if len(uuids) == 1 else None,
                    "module_id": modules[0]["manifest_index"] if len(modules) == 1 else None,
                    "module_instance": "allocate dynamically",
                    "message_type": 4,
                    "module_target": True,
                    "parameter_id": parameter_id,
                    "control_id": control_plan["control_index_within_type"],
                    "control_id_status": "derived from per-widget bytes-control index ordering",
                    "abi_version": abi_version,
                    "parameter_data_bytes": len(ipc_payload),
                    "payload_bytes": len(control_payload),
                    "extension_first_last_size_param0": (
                        (1 << 29) | (1 << 28) | len(control_payload) |
                        (parameter_id << 20)
                    ),
                    "control_payload_header_bytes": 0,
                    "control_payload_header_layout": "none; module large-config carries parameter bytes directly",
                    "payload_template_control_id_zero_not_validated": control_payload.hex(),
                    "control_id_binding_required_before_send": True,
                    "payload_sha256": hashlib.sha256(control_payload).hexdigest(),
                    "transport_note": "wire payload prepends SOF IPC4 control header; control index is the per-widget bytes index; ABI header stays host-side",
                }
            controls.append(control_plan)
            pos += len(control_data)
        widgets[name] = {
            "source_block_index": index,
            "widget_type_code": struct.unpack_from("<I", record, 4)[0],
            "private_tuple_uuids": uuids,
            "private_tuple_arrays": parse_tuple_arrays(private, audit),
            "input_audio_formats": input_formats,
            "output_audio_formats": output_formats,
            "firmware_modules": modules,
            "embedded_control_count": control_count,
            "private_tuple_data_size": len(private),
            "private_tuple_data_hex": private.hex(),
            "embedded_controls": controls,
            "record_sha256": hashlib.sha256(record).hexdigest(),
        }
        if len(modules) == 1 and input_formats and output_formats:
            module = module_by_uuid[modules[0]["uuid"]]
            input_index, input_fmt = select_format(
                input_formats, STREAM_FORMAT, name, "input")
            output_index, output_fmt = select_format(
                output_formats, STREAM_FORMAT, name, "output")
            token_values = {token: value for _kind, token, value in audio_tuples}
            is_pages = int(token_values.get(409, 0))
            ibs, obs = int(input_fmt["buffer_size"]), int(output_fmt["buffer_size"])
            cfg_matches = [cfg for cfg in module_configs[module.module_uuid]
                           if cfg["ibs"] == ibs and cfg["obs"] == obs]
            cpc = max((cfg["cpc"] for cfg in cfg_matches), default=0)
            base_words = [
                cpc, ibs, obs, is_pages,
                int(input_fmt["sampling_frequency"]), int(input_fmt["bit_depth"]),
                int(input_fmt["channel_map"]), int(input_fmt["channel_config"]),
                int(input_fmt["interleaving_style"]), int(input_fmt["format_config"]),
            ]
            widget_plan = widgets[name]
            widget_plan["selected_module_configuration"] = {
                "uuid": module.module_uuid,
                "module_id": module.index,
                "module_type": module.module_type,
                "module_init_config_type": (module.module_type >> 8) & 0xf,
                "core_id": int(token_values.get(404, 0)),
                "input_format_index": input_index,
                "input_format": input_fmt,
                "output_format_index": output_index,
                "output_format": output_fmt,
                "ibs": ibs,
                "obs": obs,
                "is_pages": is_pages,
                "manifest_ibs_obs_matches": cfg_matches,
                "cpc": cpc,
                "base_module_config_words": base_words,
                "base_module_config_bytes": 40,
                "base_module_config_hex": struct.pack("<10I", *base_words).hex(),
                "resource_estimate": module_resource_pages(module.module_type, ibs, is_pages),
            }
            output_words = [int(output_fmt[key]) for key in (
                "sampling_frequency", "bit_depth", "channel_map", "channel_config",
                "interleaving_style", "format_config")]
            base_bytes = struct.pack("<10I", *base_words)
            if name.startswith("dai-copier.DMIC."):
                node_type = int(token_values.get(1980, 0))
                dai_index = int(token_values.get(155, 0))
                nhlt = extract_hp_dmic_nhlt(args.nhlt) if args.nhlt else None
                if nhlt is None:
                    raise ValueError("--nhlt is required to serialize DMIC DAI copier init")
                nhlt_blob = bytes.fromhex(nhlt["blob_hex"])
                gateway = struct.pack("<3I", (node_type << 8) | (dai_index & 0x7),
                                      4 * ibs, len(nhlt_blob) // 4)
                init_payload = (base_bytes + struct.pack("<6I", *output_words)
                                + struct.pack("<I", 0) + gateway + nhlt_blob)
                if len(nhlt_blob) % 4:
                    raise ValueError("NHLT DMIC blob is not dword aligned")
                init_kind = "DAI copier: base + output format + feature mask + gateway + NHLT blob"
            elif name.startswith("module-copier."):
                gateway = struct.pack("<3I", 0xffffffff, 0, 1)
                init_payload = (base_bytes + struct.pack("<6I", *output_words)
                                + struct.pack("<I", 0) + gateway + struct.pack("<I", 0))
                init_kind = "module copier: base + output format + feature mask + gateway + one zero attribute"
            elif module.name == "GAIN":
                private_tokens = {token: value for _kind, token, value in audit.parse_tuples(private)}
                gain_params = [0xffffffff, int(private_tokens.get(262, 0x7fffffff)),
                               int(private_tokens.get(260, 0)), 0,
                               int(private_tokens.get(261, 0)), 0]
                init_payload = base_bytes + struct.pack("<6I", *gain_params)
                init_kind = "GAIN: base + six-word gain params"
            else:
                init_payload = base_bytes
                init_kind = "base module config only"
            widget_plan["selected_module_configuration"].update({
                "init_payload_bytes": len(init_payload),
                "init_payload_sha256": hashlib.sha256(init_payload).hexdigest(),
                "init_payload_hex": init_payload.hex(),
                "init_payload_layout": init_kind,
                "init_payload_serialization_status": "derived from local SOF IPC4 struct layout; static offline only",
                "ipc4_init_module_header": {
                    "message_type": 0,
                    "module_target": True,
                    "primary_template_without_instance": (1 << 30) | module.index,
                    "module_instance": "allocate dynamically",
                    "extension_payload_dwords": len(init_payload) // 4,
                    "runtime_pipeline_id": "allocate dynamically",
                    "core_id": int(token_values.get(404, 0)),
                    "payload_limit_bytes": 4096,
                    "within_limit": len(init_payload) <= 4096,
                },
            })

    route_pairs = {
        (audit.cstr(record[88:132]), audit.cstr(record[:44]))
        for kind, _index, record in records if kind == 4
    }
    routes = []
    for source, sink in sorted(route_pairs):
        source_plan = widgets[source]
        sink_plan = widgets[sink]
        source_module = source_plan["selected_module_configuration"]
        sink_module = sink_plan["selected_module_configuration"]
        source_values = {token: value for kind, token, value in audit.parse_tuples(
            bytes.fromhex(source_plan["private_tuple_data_hex"])) if kind in (2, 4)}
        sink_values = {token: value for kind, token, value in audit.parse_tuples(
            bytes.fromhex(sink_plan["private_tuple_data_hex"])) if kind in (2, 4)}
        if int(source_values.get(412, 0)) != 1 or int(sink_values.get(411, 0)) != 1:
            raise ValueError(f"route {source} -> {sink} is not uniquely queue-zero bindable")
        src_id, dst_id = source_module["module_id"], sink_module["module_id"]
        routes.append({
            "source": source,
            "sink": sink,
            "source_module_id": src_id,
            "source_instance": "allocate dynamically",
            "source_queue_id": 0,
            "destination_module_id": dst_id,
            "destination_instance": "allocate dynamically",
            "destination_queue_id": 0,
            "bind_primary_template_instance_zero": (1 << 30) | (5 << 24) | src_id,
            "unbind_primary_template_instance_zero": (1 << 30) | (6 << 24) | src_id,
            "bind_extension_template_instance_zero": dst_id,
            "unbind_extension_template_instance_zero": dst_id,
            "queue_ids_status": "both endpoint pin counts equal one; SOF queue allocator therefore uses queue 0",
        })

    pcm = next(record for kind, _index, record in records if kind == 7)
    link = next(record for kind, _index, record in records if kind == 10)
    dmic = next(name for name in widgets if name.startswith("dai-copier.DMIC."))
    pipeline = next(name for name in widgets if name.startswith("pipeline."))
    pipeline_number = int(pipeline.rsplit(".", 1)[1])
    dmic_private_size = struct.unpack_from("<I", next(
        record for kind, _index, record in records
        if kind == 5 and audit.cstr(record[8:52]) == dmic
    ), 128)[0]
    dmic_record = next(record for kind, _index, record in records
                       if kind == 5 and audit.cstr(record[8:52]) == dmic)
    link_private_size = struct.unpack_from("<I", link, 1652)[0]
    link_values = audit.parse_tuples(link[1656 : 1656 + link_private_size])
    pipeline_record = next(record for kind, _index, record in records
                           if kind == 5 and audit.cstr(record[8:52]) == pipeline)
    pipeline_control_count, pipeline_private_size = struct.unpack_from(
        "<II", pipeline_record, 124)
    pipeline_values = audit.parse_tuples(
        pipeline_record[132 : 132 + pipeline_private_size])
    pipeline_tokens = {token: value for kind, token, value in pipeline_values
                       if kind in (2, 4)}
    module_pages = sum(
        widget["selected_module_configuration"]["resource_estimate"]["pages"]
        for widget in widgets.values() if "selected_module_configuration" in widget
    )
    pipeline_domain_id = int(pipeline_tokens.get(419, 0))
    component_heaps = []
    component_stacks = []
    for widget in widgets.values():
        vals = audit.parse_tuples(bytes.fromhex(widget["private_tuple_data_hex"]))
        token_map = {token: value for kind, token, value in vals if kind in (2, 4)}
        component_heaps.append(int(token_map.get(421, 0)))
        component_stacks.append(int(token_map.get(420, 0)))
    pipeline_heap_bytes = sum(component_heaps)
    pipeline_stack_bytes = max(component_stacks, default=0)
    pipeline_payload = struct.pack("<7I", 0x01000007, 0, 0, 0x00030003,
                                   pipeline_domain_id, pipeline_stack_bytes,
                                   pipeline_heap_bytes)

    plan = {
        "scope": "offline static plan; not executable and not a firmware/DMA sequence",
        "firmware": {
            "name": args.firmware.name,
            "image_name": fw.image_name,
            "version": list(fw.version),
            "sha256": fw.sha256,
            "module_count": len(fw.modules),
        },
        "nhlt_dmic_capture": extract_hp_dmic_nhlt(args.nhlt) if args.nhlt else None,
        "topology": {
            "name": args.candidate.name,
            "bytes": len(topology),
            "sha256": hashlib.sha256(topology).hexdigest(),
            "template_pipeline_id": pipeline_number,
            "runtime_pipeline_id": "allocate dynamically; do not use template_pipeline_id",
            "pipeline_tuple_values": pipeline_values,
            "create_pipeline_fields": {
                "priority_token_201": pipeline_tokens.get(201),
                "core_id_token_203": pipeline_tokens.get(203),
                "low_power_token_207": pipeline_tokens.get(207),
                "dynamic_pipeline_token_206": pipeline_tokens.get(206),
                "memory_pages": module_pages,
                "memory_pages_status": "calculated for selected 48 kHz, 2-channel, 32-bit LSB integer stream from each module input buffer and module type",
                "primary_template_without_instance": (17 << 24) | (int(pipeline_tokens.get(201, 0)) << 11) | module_pages,
                "extension_template": (int(pipeline_tokens.get(203, 0)) << 20) | int(pipeline_tokens.get(207, 0)) | (1 << 29),
                "payload_bytes": len(pipeline_payload),
                "payload_dwords": len(pipeline_payload) // 4,
                "payload_hex": pipeline_payload.hex(),
                "payload_fields": {
                    "word0": "7 DWORD total + object-array flag",
                    "memory_object_header": "MEM_DATA object, 3 DWORD, last",
                    "domain_id": pipeline_domain_id,
                    "stack_bytes": pipeline_stack_bytes,
                    "heap_bytes": pipeline_heap_bytes,
                },
            },
            "pipeline_embedded_control_count": pipeline_control_count,
            "pcm": {
                "name": audit.cstr(pcm[4:48]),
                "pcm_id": struct.unpack_from("<I", pcm, 92)[0],
                "dai_id": struct.unpack_from("<I", pcm, 96)[0],
                "direction_capture": True,
            },
            "backend_link": {
                "name": audit.cstr(link[8:52]),
                "id": struct.unpack_from("<I", link, 4)[0],
                "tuples": [{"type": kind, "token": token, "value": value}
                           for kind, token, value in link_values],
            },
            "dai_widget": {
                "name": dmic,
                "tuples": [{"type": kind, "token": token, "value": value}
                           for kind, token, value in audit.parse_tuples(
                               dmic_record[132 : 132 + dmic_private_size])],
            },
            "widgets": widgets,
            "logical_routes": routes,
        },
        "runtime_id_policy": {
            "module_indexes": "look up by module UUID in this firmware image",
            "module_instances": "allocate per module at runtime",
            "queue_ids": "derive from topology pin bindings/routes",
        },
        "startup_control_updates": startup_control_updates,
        "safety_gates": [
            "no DSP writes have been generated or sent",
            "no firmware load or DMA start is authorized by this artifact",
            "requires an IPC4 boot/transport implementation and shared-controller recovery review",
        ],
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(plan, indent=2, ensure_ascii=False) + "\n")
    print(f"Wrote offline plan: {args.output}")
    print(f"Firmware {fw.image_name} {'.'.join(map(str, fw.version))}; "
          f"topology template pipeline {pipeline_number}; "
          f"logical widgets={len(widgets)} routes={len(routes)}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError, RuntimeError, StopIteration) as exc:
        print(f"FAIL: {exc}", file=sys.stderr)
        raise SystemExit(1)
