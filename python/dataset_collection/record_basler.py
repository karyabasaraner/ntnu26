#!/usr/bin/env python3
"""Phase 1 checkpoint: single Basler camera recording.

Usage:
    python record_basler.py --camera RGB1 --duration 10
    python record_basler.py --serial 12345678 --num-frames 300

Output (in --output-dir, default "output/record_basler"):
    images/000001.png, images/000002.png, ...
    timestamps.csv
"""
from __future__ import annotations

import argparse
import signal
import threading

import cv2

from dataset_collection.basler_recorder import BaslerFrame, BaslerRecorder
from dataset_collection.dataset_writer import CsvTimestampWriter, find_camera_serial, load_yaml, make_output_dir
from dataset_collection.clock import now_ns, now_wall_iso


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--config", default="config/camera_info.yaml")
    parser.add_argument("--camera", default="RGB1", help="camera name in camera_info.yaml (used to look up serial)")
    parser.add_argument("--serial", default="", help="explicit serial number; overrides --camera lookup")
    parser.add_argument("--output-dir", default="output/record_basler")
    parser.add_argument("--fps", type=float, default=30.0)
    parser.add_argument("--exposure-us", type=float, default=2000.0)
    parser.add_argument("--gain", type=float, default=1.0)
    parser.add_argument("--duration", type=float, default=0.0, help="seconds to record; 0 = until Ctrl+C")
    parser.add_argument("--num-frames", type=int, default=0, help="stop after this many frames; 0 = unbounded")
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
    images_dir = output_dir / "images"
    images_dir.mkdir()

    recorder = BaslerRecorder(
        serial_number=serial,
        exposure_time_us=args.exposure_us,
        gain=args.gain,
        fps=args.fps,
    )

    stop_event = threading.Event()
    csv_writer = CsvTimestampWriter(
        output_dir / "timestamps.csv",
        fieldnames=[
            "index", "filename", "camera_timestamp_ticks",
            "exposure_host_ns", "arrival_host_ns", "disk_write_complete_ns", "wall_time_iso",
        ],
    )

    def on_frame(frame: BaslerFrame) -> None:
        filename = f"{frame.index:06d}.png"
        # pypylon's RGB8packed conversion is RGB; cv2.imwrite expects BGR.
        cv2.imwrite(str(images_dir / filename), cv2.cvtColor(frame.image, cv2.COLOR_RGB2BGR))
        disk_write_complete_ns = now_ns()
        csv_writer.write({
            "index": frame.index,
            "filename": filename,
            "camera_timestamp_ticks": frame.camera_timestamp_ticks,
            "exposure_host_ns": frame.exposure_host_ns,
            "arrival_host_ns": frame.arrival_host_ns,
            "disk_write_complete_ns": disk_write_complete_ns,
            "wall_time_iso": now_wall_iso(),
        })
        if args.num_frames and frame.index >= args.num_frames:
            stop_event.set()

    def handle_signal(_signum: int, _frame: object) -> None:
        stop_event.set()

    signal.signal(signal.SIGINT, handle_signal)
    signal.signal(signal.SIGTERM, handle_signal)

    print(f"Recording to {output_dir} (serial={serial or '<first available>'}). Ctrl+C to stop.")
    recorder.start(on_frame)
    try:
        stop_event.wait(timeout=args.duration or None)
    finally:
        recorder.stop()
        recorder.close()
        csv_writer.close()

    print(f"Done: {recorder.frame_count} frames, {recorder.dropped_count} dropped (by BlockID gap).")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
