#!/usr/bin/env python3
"""Phase 5 checkpoint: RGB + Event + IMU, one command, one shared timeline.

Usage:
    python record.py --duration 10

Output (in --output-dir, default "output/record"):
    dataset/RGB1/images/ ... RGB4/images/, each with its own timestamps.csv
    dataset/Event/events.bin       -- decoded CD events (EventRecord-packed)
    dataset/imu.csv                -- accel + gyro samples
    dataset/timestamps.csv         -- per-stream manifest (see record_rgb_event.py)
    dataset/metadata.json          -- session-level summary (config used,
                                       duration, per-stream counts)

Every stream's timestamps share one clock domain (process monotonic clock,
see dataset_collection/clock.py) so alignment across RGB/Event/IMU is a
direct timestamp comparison, no cross-stream calibration needed for a first
pass -- see the README's clock-domain section for what this does and doesn't
account for (no drift correction over a long recording).
"""
from __future__ import annotations

import argparse
import signal
import threading
import time

from dataset_collection.dataset_writer import CsvTimestampWriter, find_camera_serial, load_yaml, make_output_dir, write_metadata
from dataset_collection.clock import now_wall_iso
from dataset_collection.event_record_io import EventRecordWriter
from dataset_collection.event_recorder import EventBatch, EventCameraRecorder
from dataset_collection.imu_recorder import ImuRecorder, ImuSample
from dataset_collection.rgb_rig import MultiBaslerRig, resolve_camera_serials


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--config", default="config/camera_info.yaml")
    parser.add_argument("--event-camera", default="Event")
    parser.add_argument("--output-dir", default="output/record")
    parser.add_argument("--fps", type=float, default=30.0)
    parser.add_argument("--exposure-us", type=float, default=2000.0)
    parser.add_argument("--gain", type=float, default=1.0)
    parser.add_argument("--bias-file", default="")
    parser.add_argument("--duration", type=float, default=0.0, help="seconds to record; 0 = until Ctrl+C")
    parser.add_argument("--skip-imu", action="store_true", help="record RGB + Event only")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    camera_info = load_yaml(args.config)
    rgb_serials = resolve_camera_serials(camera_info)
    event_serial = find_camera_serial(camera_info, args.event_camera) or ""
    imus = camera_info.get("imus", []) or []

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
        if event_bounds["first_ns"] is None:
            event_bounds["first_ns"] = int(batch.host_timestamp_ns.min())
        event_bounds["last_ns"] = int(batch.host_timestamp_ns.max())

    accel = gyro = None
    imu_csv = None
    imu_csv_lock = threading.Lock()
    if not args.skip_imu and imus:
        imu_config = imus[0]
        imu_csv = CsvTimestampWriter(
            output_dir / "imu.csv",
            fieldnames=["sensor", "index", "x", "y", "z", "host_timestamp_ns", "timestamp_source", "wall_time_iso"],
        )
        accel = ImuRecorder(
            device_id=imu_config["accel_device"],
            channels=imu_config["accel_channels"],
            scale=imu_config["accel_scale"],
            sampling_frequency=imu_config.get("accel_sampling_frequency", 0.0),
        )
        gyro = ImuRecorder(
            device_id=imu_config["gyro_device"],
            channels=imu_config["gyro_channels"],
            scale=imu_config["gyro_scale"],
            sampling_frequency=imu_config.get("gyro_sampling_frequency", 0.0),
        )
    elif not args.skip_imu:
        print("WARNING: no 'imus' entry in camera_info.yaml -- recording without IMU.")

    def make_on_imu_sample(sensor_name: str):
        def on_sample(sample: ImuSample) -> None:
            with imu_csv_lock:
                imu_csv.write({
                    "sensor": sensor_name,
                    "index": sample.index,
                    "x": sample.x,
                    "y": sample.y,
                    "z": sample.z,
                    "host_timestamp_ns": sample.host_timestamp_ns,
                    "timestamp_source": sample.timestamp_source,
                    "wall_time_iso": now_wall_iso(),
                })
        return on_sample

    stop_event = threading.Event()

    def handle_signal(_signum: int, _frame: object) -> None:
        stop_event.set()

    signal.signal(signal.SIGINT, handle_signal)
    signal.signal(signal.SIGTERM, handle_signal)

    print(f"Recording {rig.active_names} + event camera" + (" + IMU" if accel else "") + f" to {output_dir}. Ctrl+C to stop.")
    start_time = time.monotonic()
    rig.start()
    event_recorder.stream_events(on_events)
    if accel is not None and gyro is not None:
        accel.start(make_on_imu_sample("accel"))
        gyro.start(make_on_imu_sample("gyro"))

    try:
        stop_event.wait(timeout=args.duration or None)
    finally:
        rig.stop()
        event_recorder.stop()
        event_recorder.close()
        event_writer.close()
        if accel is not None:
            accel.stop()
            accel.close()
        if gyro is not None:
            gyro.stop()
            gyro.close()
        if imu_csv is not None:
            imu_csv.close()

    duration_s = max(time.monotonic() - start_time, 1e-6)

    manifest = CsvTimestampWriter(
        output_dir / "timestamps.csv",
        fieldnames=["stream", "count", "first_host_ns", "last_host_ns"],
    )
    per_stream = {}
    for name in rig.active_names:
        first_ns, last_ns = _first_last_exposure_ns(output_dir / name / "timestamps.csv")
        manifest.write({"stream": name, "count": rig.frame_count(name), "first_host_ns": first_ns, "last_host_ns": last_ns})
        per_stream[name] = {"frame_count": rig.frame_count(name), "dropped_count": rig.dropped_count(name)}
        print(f"{name}: {rig.frame_count(name)} frames, {rig.dropped_count(name)} dropped")

    manifest.write({
        "stream": "Event",
        "count": event_writer.count,
        "first_host_ns": event_bounds["first_ns"],
        "last_host_ns": event_bounds["last_ns"],
    })
    per_stream["Event"] = {"event_count": event_writer.count}
    print(f"Event: {event_writer.count} events")

    if accel is not None and gyro is not None:
        manifest.write({"stream": "IMU_accel", "count": accel.sample_count, "first_host_ns": None, "last_host_ns": None})
        manifest.write({"stream": "IMU_gyro", "count": gyro.sample_count, "first_host_ns": None, "last_host_ns": None})
        per_stream["IMU"] = {"accel_sample_count": accel.sample_count, "gyro_sample_count": gyro.sample_count}
        print(f"IMU: {accel.sample_count} accel samples, {gyro.sample_count} gyro samples")
    manifest.close()

    write_metadata(output_dir / "metadata.json", {
        "config_path": args.config,
        "duration_s": duration_s,
        "fps_requested": args.fps,
        "exposure_us": args.exposure_us,
        "gain": args.gain,
        "streams": per_stream,
    })

    print(f"Done. {output_dir / 'metadata.json'}")
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
