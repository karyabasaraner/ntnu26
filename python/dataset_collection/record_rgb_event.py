#!/usr/bin/env python3
"""Phase 4 checkpoint: RGB + Event camera, synchronized recording.

Usage:
    python record_rgb_event.py --duration 10

Output (in --output-dir, default "output/record_rgb_event"):
    dataset/RGB1/images/ ... RGB4/images/, each with its own timestamps.csv
    dataset/Event/events.bin   -- decoded CD events, packed as EventRecord
                                   (see dataset_collection/event_record_io.py),
                                   timestamped in the same shared monotonic
                                   clock domain as the RGB streams
    dataset/timestamps.csv     -- a per-stream manifest (name, count,
                                   first/last timestamp), NOT a merged
                                   per-frame/per-event table -- each stream's
                                   own detailed timestamps live alongside it
                                   (RGB*/timestamps.csv, event count/rate
                                   here). Use this to sanity-check the streams'
                                   time ranges actually overlap.

Both streams key their timestamps off dataset_collection.clock.now_ns(), so
"verify alignment" (Phase 4's task) means: load a RGB*/timestamps.csv and
Event/events.bin from the same run and confirm events with a given
timestamp land in the same real-world instant as the RGB frame with the
closest exposure_host_ns.
"""
from __future__ import annotations

import argparse
import signal
import threading
import time

from dataset_collection.dataset_writer import CsvTimestampWriter, find_camera_serial, load_yaml, make_output_dir
from dataset_collection.event_record_io import EventRecordWriter
from dataset_collection.event_recorder import EventBatch, EventCameraRecorder
from dataset_collection.rgb_rig import MultiBaslerRig, resolve_camera_serials


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--config", default="config/camera_info.yaml")
    parser.add_argument("--event-camera", default="Event")
    parser.add_argument("--output-dir", default="output/record_rgb_event")
    parser.add_argument("--fps", type=float, default=30.0)
    parser.add_argument("--exposure-us", type=float, default=2000.0)
    parser.add_argument("--gain", type=float, default=1.0)
    parser.add_argument("--bias-file", default="")
    parser.add_argument("--duration", type=float, default=0.0, help="seconds to record; 0 = until Ctrl+C")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    camera_info = load_yaml(args.config)
    rgb_serials = resolve_camera_serials(camera_info)
    event_serial = find_camera_serial(camera_info, args.event_camera) or ""

    output_dir = make_output_dir(args.output_dir, "dataset")
    rig = MultiBaslerRig(output_dir, rgb_serials, fps=args.fps, exposure_us=args.exposure_us, gain=args.gain)
    if not rig.active_names:
        print("No RGB cameras with a serial_number configured; run discover_cameras.py first.")
        return 1

    event_dir = output_dir / "Event"
    event_dir.mkdir()
    event_recorder = EventCameraRecorder(serial_number=event_serial, bias_file=args.bias_file)
    event_writer = EventRecordWriter(event_dir / "events.bin")
    event_bounds = {"first_ns": None, "last_ns": None}

    def on_events(batch: EventBatch) -> None:
        event_writer.write_batch(batch)
        if batch.host_timestamp_ns.size == 0:
            return
        first = int(batch.host_timestamp_ns.min())
        last = int(batch.host_timestamp_ns.max())
        if event_bounds["first_ns"] is None:
            event_bounds["first_ns"] = first
        event_bounds["last_ns"] = last

    stop_event = threading.Event()

    def handle_signal(_signum: int, _frame: object) -> None:
        stop_event.set()

    signal.signal(signal.SIGINT, handle_signal)
    signal.signal(signal.SIGTERM, handle_signal)

    print(f"Recording {rig.active_names} + event camera to {output_dir}. Ctrl+C to stop.")
    rig.start()
    event_recorder.stream_events(on_events)

    try:
        stop_event.wait(timeout=args.duration or None)
    finally:
        rig.stop()
        event_recorder.stop()
        event_recorder.close()
        event_writer.close()

    manifest = CsvTimestampWriter(
        output_dir / "timestamps.csv",
        fieldnames=["stream", "count", "first_host_ns", "last_host_ns"],
    )
    for name in rig.active_names:
        first_ns, last_ns = _first_last_exposure_ns(output_dir / name / "timestamps.csv")
        manifest.write({"stream": name, "count": rig.frame_count(name), "first_host_ns": first_ns, "last_host_ns": last_ns})
        print(f"{name}: {rig.frame_count(name)} frames, {rig.dropped_count(name)} dropped")
    manifest.write({
        "stream": "Event",
        "count": event_writer.count,
        "first_host_ns": event_bounds["first_ns"],
        "last_host_ns": event_bounds["last_ns"],
    })
    manifest.close()
    print(f"Event: {event_writer.count} events")

    print(f"Done. Manifest written to {output_dir / 'timestamps.csv'}")
    return 0


def _first_last_exposure_ns(csv_path) -> tuple:
    import csv

    first_ns, last_ns = None, None
    with open(csv_path, "r", newline="", encoding="utf-8") as handle:
        for row in csv.DictReader(handle):
            value = int(row["exposure_host_ns"])
            if first_ns is None:
                first_ns = value
            last_ns = value
    return first_ns, last_ns


if __name__ == "__main__":
    raise SystemExit(main())
