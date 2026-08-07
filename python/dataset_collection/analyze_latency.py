#!/usr/bin/env python3
"""Phase 3: synchronization/latency analysis.

Loads one or more timestamps.csv files produced by record_basler.py or
record_multi_rgb.py and answers "how synchronized are the cameras?" using
what's actually instrumented today:

    exposure (camera hardware clock) -> arrival at Jetson -> write to disk

"Camera trigger" is NOT included: the module currently runs the Baslers
free-running (AcquisitionFrameRate), not on an explicit software/hardware
trigger, so there is no trigger-issue timestamp to measure against yet.
Add one (e.g. a logged time for each TriggerSoftware.Execute() call, or a
GPIO pulse timestamp for a hardware trigger) before this script can plot that
stage -- see the README's Phase 3 notes.

Usage:
    # single camera
    python analyze_latency.py output/record_basler/run/timestamps.csv

    # multiple cameras -- also adds cross-camera synchronization error plots
    python analyze_latency.py \
        output/record_multi_rgb/run/RGB1/timestamps.csv \
        output/record_multi_rgb/run/RGB2/timestamps.csv \
        output/record_multi_rgb/run/RGB3/timestamps.csv \
        output/record_multi_rgb/run/RGB4/timestamps.csv

Output: latency_analysis.pdf (default alongside the first input file) plus a
printed summary.
"""
from __future__ import annotations

import argparse
import csv
from dataclasses import dataclass
from pathlib import Path

import matplotlib
matplotlib.use("Agg")  # headless-safe; no X server needed on the Jetson over SSH
import matplotlib.pyplot as plt
import numpy as np
from matplotlib.backends.backend_pdf import PdfPages


@dataclass
class CameraTimestamps:
    name: str
    exposure_ns: np.ndarray
    arrival_ns: np.ndarray
    disk_write_ns: np.ndarray


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("timestamps_csv", nargs="+", help="one or more timestamps.csv files (one per camera)")
    parser.add_argument("--output", default="", help="output PDF path; defaults next to the first input file")
    return parser.parse_args()


def load_timestamps(path: str) -> CameraTimestamps:
    csv_path = Path(path)
    exposure, arrival, disk_write = [], [], []
    with csv_path.open("r", newline="", encoding="utf-8") as handle:
        for row in csv.DictReader(handle):
            exposure.append(int(row["exposure_host_ns"]))
            arrival.append(int(row["arrival_host_ns"]))
            disk_write.append(int(row["disk_write_complete_ns"]))
    name = csv_path.parent.name if csv_path.parent.name else csv_path.stem
    return CameraTimestamps(
        name=name,
        exposure_ns=np.array(exposure, dtype=np.int64),
        arrival_ns=np.array(arrival, dtype=np.int64),
        disk_write_ns=np.array(disk_write, dtype=np.int64),
    )


def ns_to_ms(values: np.ndarray) -> np.ndarray:
    return values.astype(np.float64) / 1e6


def plot_pipeline_latency(pdf: PdfPages, camera: CameraTimestamps) -> None:
    exposure_to_arrival_ms = ns_to_ms(camera.arrival_ns - camera.exposure_ns)
    arrival_to_disk_ms = ns_to_ms(camera.disk_write_ns - camera.arrival_ns)
    end_to_end_ms = ns_to_ms(camera.disk_write_ns - camera.exposure_ns)

    fig, axes = plt.subplots(1, 3, figsize=(15, 4))
    for ax, data, title in zip(
        axes,
        [exposure_to_arrival_ms, arrival_to_disk_ms, end_to_end_ms],
        ["exposure -> arrival", "arrival -> disk write", "exposure -> disk write (end-to-end)"],
    ):
        ax.hist(data, bins=50)
        ax.set_title(f"{camera.name}: {title}")
        ax.set_xlabel("latency (ms)")
        ax.set_ylabel("frames")
        ax.axvline(float(np.median(data)), color="red", linestyle="--", label=f"median={np.median(data):.2f}ms")
        ax.legend()
    fig.tight_layout()
    pdf.savefig(fig)
    plt.close(fig)


def plot_frame_interval_jitter(pdf: PdfPages, camera: CameraTimestamps) -> None:
    if camera.exposure_ns.size < 2:
        return
    intervals_ms = ns_to_ms(np.diff(camera.exposure_ns))
    fig, ax = plt.subplots(figsize=(8, 4))
    ax.hist(intervals_ms, bins=50)
    ax.set_title(f"{camera.name}: inter-frame interval (exposure timestamps)")
    ax.set_xlabel("interval (ms)")
    ax.set_ylabel("frames")
    mean_interval = float(np.mean(intervals_ms))
    implied_fps = 1000.0 / mean_interval if mean_interval > 0 else float("nan")
    ax.axvline(mean_interval, color="red", linestyle="--", label=f"mean={mean_interval:.2f}ms ({implied_fps:.1f} fps)")
    ax.legend()
    fig.tight_layout()
    pdf.savefig(fig)
    plt.close(fig)


def nearest_match_offsets_ms(reference_ns: np.ndarray, other_ns: np.ndarray) -> np.ndarray:
    """For each timestamp in `other_ns`, finds the nearest timestamp in
    `reference_ns` and returns the signed offset (other - nearest_reference)
    in milliseconds. This is a nearest-neighbor match, not an index-aligned
    one: with free-running (not hardware-triggered) cameras, frame N on one
    camera is not guaranteed to be the same physical instant as frame N on
    another.
    """
    sorted_reference = np.sort(reference_ns)
    insert_positions = np.searchsorted(sorted_reference, other_ns)
    offsets = []
    for value, pos in zip(other_ns, insert_positions):
        candidates = []
        if pos > 0:
            candidates.append(sorted_reference[pos - 1])
        if pos < sorted_reference.size:
            candidates.append(sorted_reference[pos])
        if not candidates:
            continue
        nearest = min(candidates, key=lambda c: abs(int(c) - int(value)))
        offsets.append(value - nearest)
    return ns_to_ms(np.array(offsets, dtype=np.int64))


def plot_cross_camera_sync(pdf: PdfPages, cameras: list[CameraTimestamps]) -> None:
    if len(cameras) < 2:
        return
    reference = cameras[0]
    fig, axes = plt.subplots(1, len(cameras) - 1, figsize=(5 * (len(cameras) - 1), 4), squeeze=False)
    for ax, other in zip(axes[0], cameras[1:]):
        offsets_ms = nearest_match_offsets_ms(reference.exposure_ns, other.exposure_ns)
        ax.hist(offsets_ms, bins=50)
        ax.set_title(f"{other.name} vs {reference.name} (nearest-match)")
        ax.set_xlabel("offset (ms)")
        ax.set_ylabel("frames")
        ax.axvline(float(np.median(offsets_ms)), color="red", linestyle="--", label=f"median={np.median(offsets_ms):.3f}ms")
        ax.legend()
    fig.suptitle("Cross-camera synchronization error (exposure timestamps, nearest-neighbor matched)")
    fig.tight_layout()
    pdf.savefig(fig)
    plt.close(fig)


def print_summary(cameras: list[CameraTimestamps]) -> None:
    print("== Latency summary ==")
    for camera in cameras:
        exposure_to_arrival_ms = ns_to_ms(camera.arrival_ns - camera.exposure_ns)
        end_to_end_ms = ns_to_ms(camera.disk_write_ns - camera.exposure_ns)
        print(
            f"{camera.name}: {camera.exposure_ns.size} frames | "
            f"exposure->arrival median={np.median(exposure_to_arrival_ms):.2f}ms | "
            f"end-to-end median={np.median(end_to_end_ms):.2f}ms"
        )
    if len(cameras) >= 2:
        reference = cameras[0]
        print(f"== Cross-camera sync vs {reference.name} (nearest-match, exposure timestamps) ==")
        for other in cameras[1:]:
            offsets_ms = nearest_match_offsets_ms(reference.exposure_ns, other.exposure_ns)
            print(f"{other.name}: median offset={np.median(offsets_ms):.3f}ms, std={np.std(offsets_ms):.3f}ms")


def main() -> int:
    args = parse_args()
    cameras = [load_timestamps(path) for path in args.timestamps_csv]

    output_path = Path(args.output) if args.output else Path(args.timestamps_csv[0]).parent / "latency_analysis.pdf"
    if len(args.timestamps_csv) > 1:
        # multi-camera input: put the PDF one level up (next to RGB1/, RGB2/, ...)
        output_path = Path(args.output) if args.output else Path(args.timestamps_csv[0]).parent.parent / "latency_analysis.pdf"

    with PdfPages(output_path) as pdf:
        for camera in cameras:
            plot_pipeline_latency(pdf, camera)
            plot_frame_interval_jitter(pdf, camera)
        plot_cross_camera_sync(pdf, cameras)

    print_summary(cameras)
    print(f"\nWritten to {output_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
