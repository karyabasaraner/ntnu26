#!/usr/bin/env python3
"""Phase 1 checkpoint: single event camera recording (raw passthrough).

Usage:
    python record_event.py --duration 10
    python record_event.py --camera Event --duration 10

Output (in --output-dir, default "output/record_event"):
    events.raw       -- the sensor's native RAW stream, unmodified
    timestamps.csv   -- a session log (start/stop, device info), NOT a
                        per-event log: events.raw already carries full
                        per-event timestamps in Prophesee's own format.
"""
from __future__ import annotations

import argparse
import signal
import threading
import time

from dataset_collection.dataset_writer import CsvTimestampWriter, find_camera_serial, load_yaml, make_output_dir
from dataset_collection.clock import now_wall_iso
from dataset_collection.event_recorder import EventCameraRecorder


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--config", default="config/camera_info.yaml")
    parser.add_argument("--camera", default="Event", help="event camera name in camera_info.yaml")
    parser.add_argument("--serial", default="", help="explicit serial number; overrides --camera lookup")
    parser.add_argument("--bias-file", default="")
    parser.add_argument("--output-dir", default="output/record_event")
    parser.add_argument("--duration", type=float, default=0.0, help="seconds to record; 0 = until Ctrl+C")
    return parser.parse_args()


def resolve_serial(args: argparse.Namespace) -> str:
    if args.serial:
        return args.serial
    camera_info = load_yaml(args.config)
    return find_camera_serial(camera_info, args.camera) or ""


def main() -> int:
    args = parse_args()
    serial = resolve_serial(args)

    output_dir = make_output_dir(args.output_dir, "run")
    raw_path = output_dir / "events.raw"

    recorder = EventCameraRecorder(serial_number=serial, bias_file=args.bias_file)
    stop_event = threading.Event()

    def handle_signal(_signum: int, _frame: object) -> None:
        stop_event.set()

    signal.signal(signal.SIGINT, handle_signal)
    signal.signal(signal.SIGTERM, handle_signal)

    csv_writer = CsvTimestampWriter(output_dir / "timestamps.csv", fieldnames=["event", "wall_time_iso"])

    print(f"Recording to {raw_path} (serial={serial or '<first available>'}). Ctrl+C to stop.")
    start_wall = now_wall_iso()
    recorder.start_raw_recording(raw_path)
    csv_writer.write({"event": "start", "wall_time_iso": start_wall})
    csv_writer.flush()

    try:
        stop_event.wait(timeout=args.duration or None)
    finally:
        recorder.stop_raw_recording()
        recorder.close()
        csv_writer.write({"event": "stop", "wall_time_iso": now_wall_iso()})
        csv_writer.close()

    print(f"Done: {raw_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
