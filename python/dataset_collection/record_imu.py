#!/usr/bin/env python3
"""Phase 1 checkpoint: IMU recording (accelerometer + gyroscope).

Usage:
    python record_imu.py --duration 10

Output (in --output-dir, default "output/record_imu"):
    imu.csv   -- rows from both the accelerometer and gyroscope IIO devices,
                 distinguished by the "sensor" column, each row's
                 host_timestamp_ns already in the shared monotonic clock
                 domain (see dataset_collection/clock.py and imu_recorder.py).
"""
from __future__ import annotations

import argparse
import signal
import threading

from dataset_collection.dataset_writer import CsvTimestampWriter, load_yaml, make_output_dir
from dataset_collection.clock import now_wall_iso
from dataset_collection.imu_recorder import ImuRecorder, ImuSample


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--config", default="config/camera_info.yaml")
    parser.add_argument("--output-dir", default="output/record_imu")
    parser.add_argument("--duration", type=float, default=0.0, help="seconds to record; 0 = until Ctrl+C")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    camera_info = load_yaml(args.config)
    imus = camera_info.get("imus", []) or []
    if not imus:
        print("No 'imus' entry in camera_info.yaml")
        return 1
    imu_config = imus[0]

    output_dir = make_output_dir(args.output_dir, "run")
    csv_lock = threading.Lock()
    csv_writer = CsvTimestampWriter(
        output_dir / "imu.csv",
        fieldnames=["sensor", "index", "x", "y", "z", "host_timestamp_ns", "timestamp_source", "wall_time_iso"],
    )

    def make_on_sample(sensor_name: str):
        def on_sample(sample: ImuSample) -> None:
            with csv_lock:
                csv_writer.write({
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

    stop_event = threading.Event()

    def handle_signal(_signum: int, _frame: object) -> None:
        stop_event.set()

    signal.signal(signal.SIGINT, handle_signal)
    signal.signal(signal.SIGTERM, handle_signal)

    print(f"Recording to {output_dir}. Ctrl+C to stop.")
    accel.start(make_on_sample("accel"))
    gyro.start(make_on_sample("gyro"))
    try:
        stop_event.wait(timeout=args.duration or None)
    finally:
        accel.stop()
        gyro.stop()
        accel.close()
        gyro.close()
        csv_writer.close()

    print(f"Done: {accel.sample_count} accel samples, {gyro.sample_count} gyro samples.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
