#!/usr/bin/env python3
"""Decode a recorded events.raw file and render what it actually captured.

Reads the RAW file back through the Metavision SDK's high-level
`metavision_core.event_io.EventsIterator` (verified on-device against a
real GenX320 recording -- confirmed 320x320 resolution and (x, y, p, t)
event fields), and produces one PNG with:
  - a polarity-colored heatmap (red = "got brighter" events, blue = "got
    darker" events, accumulated over the whole recording) showing WHERE
    activity happened in the frame
  - an event-rate-over-time plot showing WHEN activity happened

This is a diagnostic/sanity-check tool, not a Phase deliverable -- it does
not touch the shared clock domain or dataset layout the other scripts use.

With --gif, also renders an animated GIF: each frame shows only the events
from one delta-t window (not accumulated across the whole recording), so it
actually plays back like a video of what the sensor saw, instead of
collapsing all the timing information into one static image. This is the
closer-to-intended way to look at event camera data.

Usage:
    python visualize_events.py --input output/record_event/run_5/events.raw
    python visualize_events.py --input <path> --output my_plot.png --delta-t-ms 20
    python visualize_events.py --input <path> --gif --delta-t-ms 20

Requires h5py (pip install h5py) for metavision_core.event_io to import,
even though HDF5 itself isn't used here -- it's an unconditional import
inside that package. --gif additionally requires Pillow (usually already
present as a matplotlib dependency; `pip install Pillow` if not).
"""
from __future__ import annotations

import argparse
from pathlib import Path

import matplotlib
matplotlib.use("Agg")  # headless: no DISPLAY over SSH, just save to file
import matplotlib.pyplot as plt
import numpy as np

from metavision_core.event_io import EventsIterator


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--input", required=True, help="path to a recorded events.raw file")
    parser.add_argument("--output", default="", help="output PNG path (default: <input>.png next to the input)")
    parser.add_argument("--delta-t-ms", type=float, default=20.0, help="chunk size in ms for reading + the rate plot's time resolution / GIF frame duration")
    parser.add_argument("--gif", action="store_true", help="also render an animated GIF (one frame per delta-t window, not accumulated)")
    parser.add_argument("--gif-output", default="", help="output GIF path (default: <input>.gif next to the input)")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    input_path = Path(args.input)
    output_path = Path(args.output) if args.output else input_path.with_suffix(".png")

    delta_t_us = args.delta_t_ms * 1000.0
    mv_iterator = EventsIterator(input_path=str(input_path), delta_t=delta_t_us)
    height, width = mv_iterator.get_size()
    print(f"Sensor size: {height}x{width}")

    heat_pos = np.zeros((height, width), dtype=np.int64)
    heat_neg = np.zeros((height, width), dtype=np.int64)
    chunk_counts: list[int] = []
    gif_frames: list[np.ndarray] = [] if args.gif else None
    total_events = 0
    total_pos = 0
    total_neg = 0

    for evs in mv_iterator:
        chunk_counts.append(int(evs.size))
        if gif_frames is not None:
            # One frame per delta-t window, NOT accumulated across the whole
            # recording -- this is what makes it play back like a video
            # instead of collapsing into one static heatmap.
            frame = np.zeros((height, width, 3), dtype=np.uint8)
        if evs.size == 0:
            if gif_frames is not None:
                gif_frames.append(frame)
            continue
        pos_mask = evs["p"] == 1
        neg_mask = ~pos_mask
        np.add.at(heat_pos, (evs["y"][pos_mask], evs["x"][pos_mask]), 1)
        np.add.at(heat_neg, (evs["y"][neg_mask], evs["x"][neg_mask]), 1)
        if gif_frames is not None:
            frame[evs["y"][pos_mask], evs["x"][pos_mask], 0] = 255  # red: got brighter
            frame[evs["y"][neg_mask], evs["x"][neg_mask], 2] = 255  # blue: got darker
            gif_frames.append(frame)
        total_events += evs.size
        total_pos += int(pos_mask.sum())
        total_neg += int(neg_mask.sum())

    duration_s = len(chunk_counts) * args.delta_t_ms / 1000.0
    print(f"Total events: {total_events} ({total_pos} positive / {total_neg} negative)")
    print(f"Duration: {duration_s:.2f}s, avg rate: {total_events / duration_s if duration_s > 0 else 0:.0f} events/s")

    if total_events == 0:
        print("WARNING: zero events decoded -- nothing to visualize. Was there real motion during recording?")

    # Polarity-colored heatmap: red channel = positive events, blue = negative,
    # log-scaled so a few very active pixels don't wash out the rest.
    heat_rgb = np.zeros((height, width, 3), dtype=np.float32)
    if heat_pos.max() > 0:
        heat_rgb[..., 0] = np.log1p(heat_pos) / np.log1p(heat_pos.max())
    if heat_neg.max() > 0:
        heat_rgb[..., 2] = np.log1p(heat_neg) / np.log1p(heat_neg.max())

    fig, (ax_heat, ax_rate) = plt.subplots(1, 2, figsize=(14, 6))

    ax_heat.imshow(heat_rgb)
    ax_heat.set_title(f"Event activity by location\n(red=brighter, blue=darker, log-scaled)\n{input_path.name}")
    ax_heat.set_xlabel("x")
    ax_heat.set_ylabel("y")

    times_ms = np.arange(len(chunk_counts)) * args.delta_t_ms
    ax_rate.plot(times_ms, chunk_counts)
    ax_rate.set_title(f"Event rate over time ({args.delta_t_ms:.0f}ms buckets)")
    ax_rate.set_xlabel("time (ms)")
    ax_rate.set_ylabel("events per bucket")

    fig.suptitle(f"{total_events} events over {duration_s:.2f}s ({total_pos} pos / {total_neg} neg)")
    fig.tight_layout()
    fig.savefig(output_path, dpi=120)
    print(f"Saved: {output_path}")

    if gif_frames is not None:
        from PIL import Image

        gif_output_path = Path(args.gif_output) if args.gif_output else input_path.with_suffix(".gif")
        pil_frames = [Image.fromarray(frame, mode="RGB") for frame in gif_frames]
        pil_frames[0].save(
            gif_output_path,
            save_all=True,
            append_images=pil_frames[1:],
            duration=args.delta_t_ms,  # ms per frame, matches the decode chunk size
            loop=0,
        )
        print(f"Saved: {gif_output_path} ({len(pil_frames)} frames @ {args.delta_t_ms:.0f}ms/frame)")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
