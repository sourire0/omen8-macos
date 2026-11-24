#!/usr/bin/env python3
"""Generate C++ byte vectors from the independently audited JSON plan."""

from __future__ import annotations

import argparse
import json
from pathlib import Path


def bytes_initializer(data: bytes) -> str:
    return ",\n    ".join(
        ", ".join(f"0x{value:02x}" for value in data[start:start + 12])
        for start in range(0, len(data), 12))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("plan", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--kernel-output", type=Path,
                        help="also emit a fixed C-array header without STL")
    args = parser.parse_args()
    plan = json.loads(args.plan.read_text())
    widgets = plan["topology"]["widgets"]
    data = {
        "kDaiCopierInit": bytes.fromhex(widgets["dai-copier.DMIC.dmic01.capture"]["selected_module_configuration"]["init_payload_hex"]),
        "kModuleCopierInit": bytes.fromhex(widgets["module-copier.12.2"]["selected_module_configuration"]["init_payload_hex"]),
        "kEqIirInit": bytes.fromhex(widgets["eqiir.12.1"]["selected_module_configuration"]["init_payload_hex"]),
        "kGainInit": bytes.fromhex(widgets["gain.12.1"]["selected_module_configuration"]["init_payload_hex"]),
        "kCreatePipelinePayload": bytes.fromhex(plan["topology"]["create_pipeline_fields"]["payload_hex"]),
    }
    eq_control = next(control["ipc4_large_config_set"]
                      for widget in widgets.values()
                      for control in widget.get("embedded_controls", [])
                      if "ipc4_large_config_set" in control)
    data["kEqIirControlPayloadTemplate"] = bytes.fromhex(
        eq_control["payload_template_control_id_zero_not_validated"])
    switch = plan["startup_control_updates"][0]
    data["kSwitchControlPayloadTemplate"] = bytes.fromhex(
        switch["payload_hex_control_id_zero_template"])

    lines = ["#pragma once", "", "#include <cstdint>", "#include <vector>", "",
             "namespace sof::ipc4::dmic::static_data {"]
    for name, payload in data.items():
        lines.append(f"inline const std::vector<uint8_t> {name}{{")
        lines.append("    " + bytes_initializer(payload))
        lines.append("};")
    lines += ["} // namespace sof::ipc4::dmic::static_data", ""]
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text("\n".join(lines))
    print(f"generated={args.output} payloads={len(data)}")
    for name, payload in data.items():
        print(f"{name}={len(payload)} bytes")
    if args.kernel_output:
        kernel_lines = ["#pragma once", "", "#include <stddef.h>",
                        "#include <stdint.h>", "",
                        "namespace sof::ipc4::dmic::kernel_data {"]
        for name, payload in data.items():
            kernel_lines.append(f"inline constexpr uint8_t {name}[]{{")
            kernel_lines.append("    " + bytes_initializer(payload))
            kernel_lines.append("};")
            kernel_lines.append(f"inline constexpr size_t {name}Bytes = sizeof({name});")
        kernel_lines += ["} // namespace sof::ipc4::dmic::kernel_data", ""]
        args.kernel_output.parent.mkdir(parents=True, exist_ok=True)
        args.kernel_output.write_text("\n".join(kernel_lines))
        print(f"generated={args.kernel_output} kernel_arrays={len(data)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
