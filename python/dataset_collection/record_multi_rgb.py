#!/usr/bin/env python3
"""Phase 2 checkpoint: all 4 Basler cameras streaming + saving simultaneously.

Usage:
    python record_multi_rgb.py --duration 10

Output (in --output-dir, default "output/record_multi_rgb"):
    RGB1/images/000001.png ..., RGB1/timestamps.csv
    RGB2/... RGB3/... RGB4/...
    summary.json   -- per-camera FPS/dropped-frame counts, CPU/RAM usage
                       over the recording, and an SSD throughput estimate
                       (total bytes written / duration).

Cameras are identified by serial number from camera_info.yaml (RGB1-4) -- run
discover_cameras.py first and fill those in. A camera with no serial filled
in is skipped with a warning rather than guessing "first available", since
that would be ambiguous with 4 cameras on the bus.
"""
from __future__ import annotations

import argparse
import signal
import threading
import time

from dataset_collection.dataset_writer import load_yaml, make_output_dir, write_metadata
from dataset_collection.resource_monitor import ResourceMonitor
from dataset_collection.rgb_rig import MultiBaslerRig, resolve_camera_serials


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--config", default="config/camera_info.yaml")
    parser.add_argument("--output-dir", default="output/record_multi_rgb")
    parser.add_argument("--fps", type=float, default=30.0)
    parser.add_argument("--exposure-us", type=float, default=2000.0)
    parser.add_argument("--gain", type=float, default=1.0)
    parser.add_argument("--duration", type=float, default=0.0, help="seconds to record; 0 = until Ctrl+C")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    serials = resolve_camera_serials(load_yaml(args.config))

    output_dir = make_output_dir(args.output_dir, "run")
    rig = MultiBaslerRig(output_dir, serials, fps=args.fps, exposure_us=args.exposure_us, gain=args.gain)
    if not rig.active_names:
        print("No cameras with a serial_number configured; run discover_cameras.py first.")
        return 1

    stop_event = threading.Event()

    def handle_signal(_signum: int, _frame: object) -> None:
        stop_event.set()

    signal.signal(signal.SIGINT, handle_signal)
    signal.signal(signal.SIGTERM, handle_signal)

    monitor = ResourceMonitor(interval_s=1.0)

    print(f"Recording {rig.active_names} to {output_dir}. Ctrl+C to stop.")
    start_time = time.monotonic()
    monitor.start()
    rig.start()

    try:
        stop_event.wait(timeout=args.duration or None)
    finally:
        rig.stop()
        monitor.stop()

    duration_s = max(time.monotonic() - start_time, 1e-6)

    per_camera = {}
    total_bytes = 0
    for name in rig.active_names:
        bytes_written = rig.dir_size_bytes(name)
        total_bytes += bytes_written
        per_camera[name] = {
            "serial_number": rig.serials[name],
            "frame_count": rig.frame_count(name),
            "dropped_count": rig.dropped_count(name),
            "fps_achieved": rig.frame_count(name) / duration_s,
            "bytes_written": bytes_written,
        }
        print(
            f"{name}: {rig.frame_count(name)} frames, {rig.dropped_count(name)} dropped, "
            f"{per_camera[name]['fps_achieved']:.2f} fps"
        )

    write_metadata(output_dir / "summary.json", {
        "duration_s": duration_s,
        "cameras": per_camera,
        "ssd_write_mb_per_s": (total_bytes / (1024 * 1024)) / duration_s,
        "resource_usage": monitor.summary(),
    })

    print(f"Done. Summary written to {output_dir / 'summary.json'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
