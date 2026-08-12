#!/usr/bin/env python3
"""Phase 0 checkpoint: hardware validation.

Enumerates connected Basler cameras and Prophesee event cameras, and checks
that the configured IMU IIO devices are present with their expected
channels. Prints a report, and writes the raw discovery results to
<config>.discovered.yaml next to the config for you to cross-reference by
hand into camera_info.yaml's RGB1/RGB2/RGB3/RGB4/Event slots.

This deliberately does NOT auto-assign discovered serials into RGB1-4 slots:
GigE discovery order is not guaranteed to match physical camera position, and
silently getting that wrong would poison every later phase (Phase 2's
per-camera identification, Phase 4/5's dataset layout). Cross-reference by
IP address / by covering one lens at a time, or use --auto-assign only once
you've verified the ordering another way.

Usage:
    python discover_cameras.py --config config/camera_info.yaml
    python discover_cameras.py --config config/camera_info.yaml --auto-assign
"""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

import yaml

from dataset_collection.discovery import (
    check_imu_device,
    discover_baslers,
    discover_event_cameras,
)
from dataset_collection.dataset_writer import load_yaml


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--config", default="config/camera_info.yaml", help="camera_info.yaml to read/update")
    parser.add_argument(
        "--auto-assign",
        action="store_true",
        help="fill empty serial_number slots in discovery order (verify physical "
        "ordering yourself first -- see module docstring)",
    )
    return parser.parse_args()


def report_baslers() -> list:
    print("== Basler cameras ==")
    try:
        baslers = discover_baslers()
    except ImportError as exc:
        print(f"  SKIPPED: {exc}")
        return []
    if not baslers:
        print("  none found")
    for device in baslers:
        print(f"  serial={device.serial_number} ip={device.ip_address} model={device.model_name}")
    return baslers


def report_event_cameras() -> list:
    print("== Event cameras ==")
    try:
        event_cameras = discover_event_cameras()
    except ImportError as exc:
        print(f"  SKIPPED: {exc}")
        return []
    if not event_cameras:
        print("  none found")
    for device in event_cameras:
        print(f"  serial={device.serial_number}")

    serials = [device.serial_number for device in event_cameras]
    if len(serials) != len(set(serials)):
        print(
            "  WARNING: two or more event cameras reported the IDENTICAL serial string above. "
            "This has been seen on CSI-attached GenX320 pairs via the Prophesee HAL plugin -- "
            "Camera.from_serial() may not actually distinguish them, meaning both Event1/Event2 "
            "could end up opening the same physical device. Checking this needs physically "
            "disconnecting one camera at a time (unlike GigE Baslers, covering the lens doesn't "
            "change what a CSI device reports at discovery time) and re-running this script to "
            "see whether the identifier actually changes -- may need Prophesee's docs/support if "
            "it doesn't, before Phase 4/5 can trust having both event cameras active at once."
        )
    return event_cameras


def report_imus(camera_info: dict) -> None:
    print("== IMU ==")
    for imu in camera_info.get("imus", []) or []:
        for role, key in (("accel", "accel_device"), ("gyro", "gyro_device")):
            device_id = imu.get(key)
            if not device_id:
                continue
            channels = ["accel_x", "accel_y", "accel_z"] if role == "accel" else ["anglvel_x", "anglvel_y", "anglvel_z"]
            result = check_imu_device(device_id, channels)
            status = "OK" if result.found and result.error is None else f"PROBLEM: {result.error}"
            print(f"  {role} ({device_id}): {status}")


def write_discovered(config_path: Path, baslers: list, event_cameras: list) -> None:
    discovered_path = config_path.with_suffix(".discovered.yaml")
    payload = {
        "cameras": [
            {"serial_number": d.serial_number, "ip_address": d.ip_address, "model_name": d.model_name}
            for d in baslers
        ],
        "event_cameras": [{"serial_number": d.serial_number} for d in event_cameras],
    }
    discovered_path.write_text(yaml.safe_dump(payload, sort_keys=False), encoding="utf-8")
    print(f"\nRaw discovery results written to {discovered_path}")


def auto_assign(config_path: Path, camera_info: dict, baslers: list, event_cameras: list) -> None:
    cameras = camera_info.get("cameras", []) or []
    # Only consider devices not already assigned to some OTHER slot in the
    # file. Previously zipped empty_camera_slots against ALL discovered
    # devices regardless of whether they were already in use elsewhere --
    # happened to work by luck of discovery order when only one slot was
    # empty, but could silently assign an already-used serial into an empty
    # slot (creating a duplicate) if that order ever changed. This also now
    # correctly protects the event cameras' known duplicate-serial situation:
    # since both discovered event cameras report the SAME serial and Event1
    # already claims it, Event2's empty slot is correctly left untouched
    # rather than getting that same duplicate serial assigned into it too.
    already_assigned = {entry.get("serial_number") for entry in cameras if entry.get("serial_number")}
    empty_camera_slots = [entry for entry in cameras if not entry.get("serial_number")]
    unassigned_baslers = [device for device in baslers if device.serial_number not in already_assigned]
    for entry, device in zip(empty_camera_slots, unassigned_baslers):
        entry["serial_number"] = device.serial_number
        entry["ip_address"] = device.ip_address
        entry["model_name"] = device.model_name

    event_entries = camera_info.get("event_cameras", []) or []
    already_assigned_events = {entry.get("serial_number") for entry in event_entries if entry.get("serial_number")}
    empty_event_slots = [entry for entry in event_entries if not entry.get("serial_number")]
    unassigned_events = [device for device in event_cameras if device.serial_number not in already_assigned_events]
    for entry, device in zip(empty_event_slots, unassigned_events):
        entry["serial_number"] = device.serial_number

    config_path.write_text(yaml.safe_dump(camera_info, sort_keys=False), encoding="utf-8")
    print(f"\n--auto-assign: wrote discovered serials into {config_path} in discovery order.")
    print("Verify this matches physical layout before trusting RGB1-4 in later phases.")


def main() -> int:
    args = parse_args()
    config_path = Path(args.config)
    if not config_path.exists():
        print(f"Config not found: {config_path}", file=sys.stderr)
        return 1

    camera_info = load_yaml(config_path)

    baslers = report_baslers()
    print()
    event_cameras = report_event_cameras()
    print()
    report_imus(camera_info)

    write_discovered(config_path, baslers, event_cameras)

    if args.auto_assign:
        auto_assign(config_path, camera_info, baslers, event_cameras)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
