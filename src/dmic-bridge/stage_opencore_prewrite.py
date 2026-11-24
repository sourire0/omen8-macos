#!/usr/bin/env python3
"""Retired: standalone HPIPC4 PCI deployment conflicts with AppleHDA."""

from __future__ import annotations

import argparse
import hashlib
import plistlib
import shutil
import subprocess
from pathlib import Path

from prepare_opencore_prewrite import make_candidate_config


def copy_bundle(source: Path, destination: Path, executable_name: str) -> None:
    source = source.resolve(strict=True)
    executable = source / "Contents" / "MacOS" / executable_name
    info = source / "Contents" / "Info.plist"
    if not executable.is_file() or not info.is_file():
        raise ValueError(f"incomplete bundle: {source}")
    shutil.copytree(source, destination, dirs_exist_ok=True)


def validate_bundle_metadata(
    project: Path, output_root: Path, source_oc_directory: Path
) -> None:
    kext_info_path = (
        output_root / "EFI" / "OC" / "Kexts" / "HPIPC4.kext"
        / "Contents" / "Info.plist"
    )
    hal_info_path = (
        output_root / "Library" / "Audio" / "Plug-Ins" / "HAL"
        / "HPIPC4Audio.driver" / "Contents" / "Info.plist"
    )
    kext_info = plistlib.loads(kext_info_path.read_bytes())
    hal_info = plistlib.loads(hal_info_path.read_bytes())
    if (kext_info.get("CFBundlePackageType") != "KEXT" or
            kext_info.get("CFBundleExecutable") != "HPIPC4"):
        raise ValueError("candidate KEXT bundle metadata is inconsistent")
    personalities = kext_info.get("IOKitPersonalities", {})
    if not isinstance(personalities, dict) or len(personalities) != 1:
        raise ValueError("candidate KEXT must have exactly one personality")
    personality = next(iter(personalities.values()))
    if not isinstance(personality, dict):
        raise ValueError("candidate KEXT personality is malformed")
    pci_match = personality.get("IOPCIMatch")
    if (personality.get("CFBundleIdentifier") != "org.sof.HPIPC4" or
            personality.get("IOClass") != "HPIPC4Controller" or
            personality.get("IOProviderClass") != "IOPCIDevice" or
            personality.get("IOMatchCategory") != "HPIPC4Controller" or
            personality.get("IOProbeScore") != 1000 or
            not isinstance(pci_match, str) or pci_match.lower() != "0x51c88086"):
        raise ValueError("candidate KEXT PCI match is not 8086:51C8")

    controller_source = (project / "HPIPC4Controller.cpp").read_text()
    expected_checks = (
        "kTargetPciId = 0x51c88086",
        "kTargetSubsystemVendor = 0x103c",
        "kTargetSubsystem = 0x8a13",
    )
    if not all(check in controller_source for check in expected_checks):
        raise ValueError("controller PCI/subsystem guard differs from target IDs")
    if (hal_info.get("CFBundleExecutable") != "HPIPC4Audio" or
            hal_info.get("CFBundleIdentifier") != "com.hp.ipc4.dmic"):
        raise ValueError("HAL plug-in bundle metadata is inconsistent")
    factories = hal_info.get("CFPlugInFactories", {})
    types = hal_info.get("CFPlugInTypes", {})
    factory_uuid = "F21DE995-A060-4765-B22F-0C65802C4A2F"
    audio_server_plugin_type = "443ABAB8-E7B3-491A-B985-BEB9187030DB"
    if (not isinstance(factories, dict) or
            factories.get(factory_uuid) != "HPIPC4Audio_Create" or
            not isinstance(types, dict) or
            factory_uuid not in types.get(audio_server_plugin_type, [])):
        raise ValueError("HAL plug-in factory/type metadata is incomplete")
    executable = (
        output_root / "Library" / "Audio" / "Plug-Ins" / "HAL"
        / "HPIPC4Audio.driver" / "Contents" / "MacOS" / "HPIPC4Audio"
    )
    symbol_check = subprocess.run(
        ["nm", "-gU", str(executable)], check=True, capture_output=True,
        text=True,
    )
    if "_HPIPC4Audio_Create" not in symbol_check.stdout:
        raise ValueError("HAL executable does not export its declared factory")

    probe_path = (
        source_oc_directory / "Kexts" / "HPDMICProbe.kext" / "Contents"
        / "Info.plist"
    )
    probe_info = plistlib.loads(probe_path.read_bytes())
    probe_personalities = probe_info.get("IOKitPersonalities", {})
    if not isinstance(probe_personalities, dict) or len(probe_personalities) != 1:
        raise ValueError("MULL HPDMICProbe personality is missing or ambiguous")
    probe = next(iter(probe_personalities.values()))
    if (not isinstance(probe, dict) or
            probe.get("IOProviderClass") != "IOResources" or
            probe.get("IOResourceMatch") != "IOKit" or
            "IOPCIMatch" in probe):
        raise ValueError("HPDMICProbe has an unexpected PCI match")


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Retired standalone PCI staging path; DMIC now embeds under SimpleHDA"
    )
    parser.add_argument("source_config", type=Path)
    parser.add_argument("--project", type=Path, default=Path(__file__).parent)
    parser.add_argument("--enable-candidate", action="store_true")
    args = parser.parse_args()
    parser.error(
        "standalone HPIPC4 deployment is retired; use `make appledha-dmic-module` "
        "to embed the AppleHDA child service in SimpleHDA"
    )

    project = args.project.resolve(strict=True)
    source_config = args.source_config.expanduser().resolve(strict=True)
    build_path = project / "build"
    if build_path.is_symlink():
        parser.error("project build directory must not be a symlink")
    build = build_path.resolve(strict=True)
    output_name = (
        "HPIPC4-prewrite-enabled" if args.enable_candidate
        else "HPIPC4-prewrite"
    )
    output_root = build / output_name
    if output_root.is_symlink():
        parser.error("staging root must not be a symlink")
    if source_config == output_root or output_root in source_config.parents:
        parser.error("source config must be outside the generated staging tree")

    try:
        candidate_config, baseline_hash = make_candidate_config(
            source_config.read_bytes(), enabled=args.enable_candidate
        )
        candidate = plistlib.loads(candidate_config)
        entry = next(
            item for item in candidate["Kernel"]["Add"]
            if item.get("BundlePath") == "HPIPC4.kext"
        )
        expected_entry = {
            "Arch": "x86_64",
            "BundlePath": "HPIPC4.kext",
            "ExecutablePath": "Contents/MacOS/HPIPC4",
            "PlistPath": "Contents/Info.plist",
        }
        if any(entry.get(key) != value for key, value in expected_entry.items()):
            raise ValueError("OpenCore Kernel/Add metadata does not match candidate")
        if entry.get("Enabled") is not args.enable_candidate:
            raise ValueError("staged KEXT Enabled state is inconsistent")

        kext_source = project / "build" / "HPIPC4Candidate.kext"
        hal_source = project / "build" / "HPIPC4Audio.driver"
        copy_bundle(
            kext_source,
            output_root / "EFI" / "OC" / "Kexts" / "HPIPC4.kext",
            "HPIPC4",
        )
        copy_bundle(
            hal_source,
            output_root / "Library" / "Audio" / "Plug-Ins" / "HAL" / "HPIPC4Audio.driver",
            "HPIPC4Audio",
        )
        validate_bundle_metadata(project, output_root, source_config.parent)
        config_path = output_root / "EFI" / "OC" / "config.plist"
        config_path.parent.mkdir(parents=True, exist_ok=True)
        config_path.write_bytes(candidate_config)
    except (OSError, ValueError, KeyError, StopIteration, plistlib.InvalidFileException) as error:
        parser.error(str(error))

    print(f"source_config={source_config}")
    print(f"source_config_sha256={baseline_hash}")
    print(f"staged_root={output_root}")
    print(f"HPIPC4_enabled={str(args.enable_candidate).lower()}")
    for relative in (
        "EFI/OC/Kexts/HPIPC4.kext/Contents/MacOS/HPIPC4",
        "Library/Audio/Plug-Ins/HAL/HPIPC4Audio.driver/Contents/MacOS/HPIPC4Audio",
        "EFI/OC/config.plist",
    ):
        path = output_root / relative
        print(f"{relative} sha256={hashlib.sha256(path.read_bytes()).hexdigest()}")
    print("This is a worktree review package; no EFI path is written.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
