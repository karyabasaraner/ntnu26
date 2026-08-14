#!/usr/bin/env python3
"""Combined visualization for a full recording session -- RGB cameras as
playable videos, the event camera(s) as a playable video too, and IMU
accel/gyro as time-series graphs, all from one command.

Works on the output directory of record.py, record_rgb_event.py, or
record_multi_rgb.py (they all share the same RGB1-4/EventN/imu.csv layout,
just with different subsets present) -- auto-detects whatever streams
actually exist in --input-dir and skips the rest, so this doesn't care
which Phase produced the data.

Output (in --output-dir, default <input-dir>/visualization):
    RGB1.mp4 ... RGB4.mp4   -- one video per RGB camera found, built directly
                               from that camera's saved PNG frames in
                               chronological (filename) order
    Event1.mp4, Event2.mp4  -- one video per event camera found, decoded
                               from events.bin (the EventRecord binary format
                               written by stream_events(), see
                               event_record_io.py) -- NOT the raw .raw format
                               visualize_events.py reads, so this does NOT
                               require the Metavision SDK to be installed at
                               all, only numpy/opencv
    imu.png                 -- accel + gyro x/y/z over time, one plot each

RGB video fps defaults to each camera's own ACTUAL achieved fps (computed
from its timestamps.csv, first/last exposure_host_ns divided by frame
count) rather than trusting whatever --fps was requested at record time --
same "measure, don't just trust the config" approach used for fps_achieved
elsewhere in this toolkit. Override with --fps if you want all cameras
forced to one fixed rate instead (e.g. for side-by-side comparison).

Event video denoising defaults (--event-delta-t-ms 45, --denoise-us 10000,
--denoise-radius 10, --denoise-min-neighbors 1) match visualize_events.py's
empirically-tuned defaults -- reuses the exact same filter via
dataset_collection/event_denoise.py (split out specifically so it doesn't
require the Metavision SDK import visualize_events.py needs). Pass
--denoise-us 0 for the raw, undenoised decode.

Sensor resolution for the event video defaults to 320x320 (confirmed GenX320
resolution on this hardware) -- override with --event-width/--event-height
if a different sensor is ever used.

Usage:
    python visualize_dataset.py --input-dir output/record/dataset_7
    python visualize_dataset.py --input-dir output/record_multi_rgb/run_3
    python visualize_dataset.py --input-dir <path> --fps 30
    python visualize_dataset.py --input-dir <path> --denoise-us 0

Requires opencv-python (already a dependency, used elsewhere for RGB image
I/O) for video writing -- no new dependency beyond what this toolkit already
needs. Videos are written with the 'mp4v' fourcc; if playback is finicky in
a specific player, re-encode with ffmpeg (not done here to avoid adding a
new dependency for something most players handle fine).
"""
from __future__ import annotations

import argparse
import csv
from pathlib import Path
from typing import Optional

import cv2
import matplotlib
matplotlib.use("Agg")  # headless: no DISPLAY over SSH, just save to file
import matplotlib.pyplot as plt
import numpy as np

from dataset_collection.event_denoise import denoise_keep_mask
from dataset_collection.event_record_io import EVENT_RECORD_DTYPE

CAMERA_NAMES = ["RGB1", "RGB2", "RGB3", "RGB4"]
EVENT_CAMERA_NAMES = ["Event1", "Event2"]


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--input-dir", required=True, help="a record.py/record_rgb_event.py/record_multi_rgb.py output dir (e.g. output/record/dataset_7)")
    parser.add_argument("--output-dir", default="", help="default: <input-dir>/visualization")
    parser.add_argument(
        "--fps", type=float, default=0.0,
        help="RGB video fps; 0 (default) = auto-compute each camera's own ACTUAL achieved fps "
        "from its timestamps.csv instead of trusting the configured target.",
    )
    parser.add_argument("--event-delta-t-ms", type=float, default=45.0, help="event video frame duration in ms (default 45ms, matches visualize_events.py's tuned default)")
    parser.add_argument(
        "--denoise-us", type=float, default=10000.0,
        help="drop events with no spatially/temporally-correlated neighbor within this many "
        "microseconds (default 10000 = 10ms, on by default; pass 0 to disable). See "
        "dataset_collection/event_denoise.py for details.",
    )
    parser.add_argument("--denoise-radius", type=int, default=10, help="neighborhood radius in pixels for --denoise-us (default 10, matches visualize_events.py's tuned default)")
    parser.add_argument("--denoise-min-neighbors", type=int, default=1, help="require at least this many distinct recently-active neighbors (default 1)")
    parser.add_argument("--event-width", type=int, default=320, help="event sensor width in pixels (default 320, the confirmed GenX320 resolution)")
    parser.add_argument("--event-height", type=int, default=320, help="event sensor height in pixels (default 320)")
    parser.add_argument(
        "--event-dot-radius", type=int, default=1,
        help="dilate each event pixel into a (2r+1)x(2r+1) blob before encoding (default 1 = 3x3). "
        "Confirmed necessary: single-pixel colored dots on an otherwise-black frame get badly "
        "washed out by video codec chroma subsampling (max channel value ~120/255 even with "
        "denoising off) -- dilating makes events survive compression AND actually be visible to "
        "the eye at normal playback size. Pass 0 to disable and write raw single-pixel events.",
    )
    return parser.parse_args()


def _compute_achieved_fps(timestamps_csv: Path, frame_count: int, fallback: float = 30.0) -> float:
    if not timestamps_csv.exists() or frame_count < 2:
        return fallback
    first_ns = last_ns = None
    with timestamps_csv.open("r", newline="", encoding="utf-8") as handle:
        for row in csv.DictReader(handle):
            value = int(row["exposure_host_ns"])
            if first_ns is None:
                first_ns = value
            last_ns = value
    if first_ns is None or last_ns is None or last_ns <= first_ns:
        return fallback
    duration_s = (last_ns - first_ns) / 1e9
    return max(frame_count - 1, 1) / duration_s


def render_rgb_video(camera_dir: Path, output_path: Path, fps_override: float) -> Optional[dict]:
    images_dir = camera_dir / "images"
    if not images_dir.is_dir():
        return None
    frame_paths = sorted(images_dir.glob("*.png"))  # zero-padded filenames sort chronologically
    if not frame_paths:
        return None

    first_frame = cv2.imread(str(frame_paths[0]))
    if first_frame is None:
        print(f"WARNING: couldn't read {frame_paths[0]} -- skipping {camera_dir.name}")
        return None
    height, width = first_frame.shape[:2]

    fps = fps_override if fps_override > 0 else _compute_achieved_fps(camera_dir / "timestamps.csv", len(frame_paths))

    fourcc = cv2.VideoWriter_fourcc(*"mp4v")
    writer = cv2.VideoWriter(str(output_path), fourcc, fps, (width, height))
    written = 0
    for frame_path in frame_paths:
        # PNGs were saved via cv2.imwrite(..., cv2.cvtColor(rgb, COLOR_RGB2BGR))
        # in rgb_rig.py, so cv2.imread() here reads them back as the BGR
        # arrays cv2.VideoWriter itself expects -- no channel-order fixup
        # needed, unlike the event video below where we build frames from
        # scratch.
        frame = cv2.imread(str(frame_path))
        if frame is None:
            continue  # corrupted/unreadable frame -- skip rather than abort the whole video
        writer.write(frame)
        written += 1
    writer.release()
    return {"frames": written, "fps": fps, "width": width, "height": height}


def render_event_video(event_dir: Path, output_path: Path, args: argparse.Namespace) -> Optional[dict]:
    events_bin = event_dir / "events.bin"
    if not events_bin.exists():
        return None
    records = np.fromfile(events_bin, dtype=EVENT_RECORD_DTYPE)
    if records.size == 0:
        return None

    width, height = args.event_width, args.event_height
    # Defensive, even though stream_events() already filters out-of-bounds
    # coordinates before writing to disk (confirmed necessary on real
    # hardware for the RAW-decode path in visualize_events.py) -- cheap
    # insurance against indexing a frame array out of bounds.
    valid = (records["x"] < width) & (records["y"] < height)
    if not valid.all():
        records = records[valid]
    if records.size == 0:
        return None

    # Should already be chronological (stream_events() processes SDK
    # chunks in order), but sort defensively rather than assume -- cheap
    # relative to everything else here.
    sort_idx = np.argsort(records["timestamp_ns"], kind="stable")
    records = records[sort_idx]
    ts = records["timestamp_ns"]

    delta_t_ns = int(args.event_delta_t_ms * 1e6)
    t0, t1 = int(ts[0]), int(ts[-1])
    n_chunks = max((t1 - t0) // delta_t_ns + 1, 1)
    # searchsorted against chunk boundaries instead of a fresh boolean mask
    # over the full array per chunk -- matters at real recording scale
    # (millions of events, confirmed on real hardware: 9.7M+ events in a
    # single 10s clip).
    boundaries = t0 + np.arange(n_chunks + 1) * delta_t_ns
    chunk_starts = np.searchsorted(ts, boundaries[:-1], side="left")
    chunk_ends = np.searchsorted(ts, boundaries[1:], side="left")

    denoise_enabled = args.denoise_us > 0.0
    if denoise_enabled:
        last_active = np.full((height, width), -1e15, dtype=np.float64)

    fourcc = cv2.VideoWriter_fourcc(*"mp4v")
    video_fps = 1000.0 / args.event_delta_t_ms
    writer = cv2.VideoWriter(str(output_path), fourcc, video_fps, (width, height))
    dilate_kernel = (
        np.ones((2 * args.event_dot_radius + 1, 2 * args.event_dot_radius + 1), dtype=np.uint8)
        if args.event_dot_radius > 0 else None
    )

    total_kept = 0
    total_dropped_denoise = 0
    for start, end in zip(chunk_starts, chunk_ends):
        # BGR order (OpenCV convention, not RGB) since this frame goes
        # straight into cv2.VideoWriter -- channel 2 = red, channel 0 =
        # blue, matching visualize_events.py's red=brighter/blue=darker
        # convention but with the channels swapped for OpenCV.
        frame = np.zeros((height, width, 3), dtype=np.uint8)
        if end > start:
            chunk = records[start:end]
            xs, ys, ps = chunk["x"], chunk["y"], chunk["polarity"]
            chunk_ts = ts[start:end].astype(np.float64)

            if denoise_enabled:
                keep = denoise_keep_mask(
                    xs, ys, chunk_ts, last_active, args.denoise_us, args.denoise_radius,
                    min_neighbors=args.denoise_min_neighbors,
                )
                total_dropped_denoise += int((~keep).sum())
                xs, ys, ps = xs[keep], ys[keep], ps[keep]

            pos_mask = ps == 1
            frame[ys[pos_mask], xs[pos_mask], 2] = 255  # red: got brighter
            frame[ys[~pos_mask], xs[~pos_mask], 0] = 255  # blue: got darker
            total_kept += int(xs.size)
        if dilate_kernel is not None:
            # Confirmed necessary: sparse single-pixel colored dots on an
            # otherwise-black frame get badly washed out by the video
            # codec's chroma subsampling (max channel value only reached
            # ~120/255 even with denoising off, before this fix). Growing
            # each event into a small solid blob first survives compression
            # and is also just more visible to the eye at normal playback
            # size -- a single lit pixel in a 320x320 frame is nearly
            # invisible regardless of compression.
            frame = cv2.dilate(frame, dilate_kernel)
        writer.write(frame)
    writer.release()

    return {
        "total_events": int(records.size),
        "kept_after_denoise": total_kept if denoise_enabled else int(records.size),
        "dropped_by_denoise": total_dropped_denoise,
        "frames": int(n_chunks),
    }


def render_imu_graphs(dataset_dir: Path, output_path: Path) -> Optional[dict]:
    imu_csv = dataset_dir / "imu.csv"
    if not imu_csv.exists():
        return None

    series = {"accel": {"t": [], "x": [], "y": [], "z": []}, "gyro": {"t": [], "x": [], "y": [], "z": []}}
    with imu_csv.open("r", newline="", encoding="utf-8") as handle:
        for row in csv.DictReader(handle):
            sensor = row["sensor"]
            if sensor not in series:
                continue
            series[sensor]["t"].append(int(row["host_timestamp_ns"]))
            series[sensor]["x"].append(float(row["x"]))
            series[sensor]["y"].append(float(row["y"]))
            series[sensor]["z"].append(float(row["z"]))

    if not series["accel"]["t"] and not series["gyro"]["t"]:
        return None

    fig, axes = plt.subplots(2, 1, figsize=(12, 8))
    for ax, (sensor, title, ylabel) in zip(
        axes, [("accel", "Accelerometer", "m/s^2"), ("gyro", "Gyroscope", "rad/s")],
    ):
        data = series[sensor]
        if not data["t"]:
            ax.set_title(f"{title} (no samples)")
            continue
        t0 = data["t"][0]
        elapsed_s = [(t - t0) / 1e9 for t in data["t"]]
        ax.plot(elapsed_s, data["x"], label="x", linewidth=0.8)
        ax.plot(elapsed_s, data["y"], label="y", linewidth=0.8)
        ax.plot(elapsed_s, data["z"], label="z", linewidth=0.8)
        ax.set_title(f"{title} ({len(data['t'])} samples)")
        ax.set_xlabel("elapsed time (s)")
        ax.set_ylabel(ylabel)
        ax.legend()
    fig.tight_layout()
    fig.savefig(output_path, dpi=120)
    plt.close(fig)

    return {"accel_samples": len(series["accel"]["t"]), "gyro_samples": len(series["gyro"]["t"])}


def main() -> int:
    args = parse_args()
    input_dir = Path(args.input_dir)
    if not input_dir.is_dir():
        print(f"Input dir not found: {input_dir}")
        return 1
    output_dir = Path(args.output_dir) if args.output_dir else input_dir / "visualization"
    output_dir.mkdir(parents=True, exist_ok=True)

    print(f"Reading dataset from {input_dir}")
    print(f"Writing visualizations to {output_dir}\n")

    found_anything = False

    for name in CAMERA_NAMES:
        camera_dir = input_dir / name
        if not camera_dir.is_dir():
            continue
        output_path = output_dir / f"{name}.mp4"
        info = render_rgb_video(camera_dir, output_path, args.fps)
        if info is None:
            print(f"{name}: no images found, skipped")
            continue
        found_anything = True
        print(f"{name}: {info['frames']} frames @ {info['fps']:.2f}fps ({info['width']}x{info['height']}) -> {output_path}")

    for name in EVENT_CAMERA_NAMES:
        event_dir = input_dir / name
        if not event_dir.is_dir():
            continue
        output_path = output_dir / f"{name}.mp4"
        info = render_event_video(event_dir, output_path, args)
        if info is None:
            print(f"{name}: no events.bin found or empty, skipped")
            continue
        found_anything = True
        denoise_note = (
            f", {info['dropped_by_denoise']} dropped by denoise" if args.denoise_us > 0 else ""
        )
        print(f"{name}: {info['total_events']} events, {info['frames']} frames{denoise_note} -> {output_path}")

    imu_output_path = output_dir / "imu.png"
    imu_info = render_imu_graphs(input_dir, imu_output_path)
    if imu_info is None:
        print("IMU: no imu.csv found, skipped")
    else:
        found_anything = True
        print(f"IMU: {imu_info['accel_samples']} accel + {imu_info['gyro_samples']} gyro samples -> {imu_output_path}")

    if not found_anything:
        print("\nNothing found to visualize -- check --input-dir points at an actual recording's output directory.")
        return 1

    print(f"\nDone. Visualizations in {output_dir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
