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

With --denoise-us, drops events with no spatially/temporally-correlated
neighbor (a simple "activity filter") before rendering -- real motion lights
up a neighborhood of pixels together in a short window, isolated background
noise doesn't. Implemented directly here rather than via the SDK's own
filtering: confirmed via live introspection on real hardware that
metavision_sdk_cv isn't installed on this SDK build, and this GenX320's HAL
plugin doesn't support its I_EventTrailFilterModule
(device.get_i_event_trail_filter_module() returns None) or expose an
activity-filter accessor at all -- so there's no SDK-level denoising path
available on this hardware/build, hence the DIY version here. This only
affects the visualization, not what's on disk -- it can't be applied
retroactively to change what a HAL-level filter would (a HAL filter would
change what gets recorded in the first place; this one just changes what
gets drawn from an already-recorded file).

Usage:
    python visualize_events.py --input output/record_event/run_5/events.raw
    python visualize_events.py --input <path> --output my_plot.png --delta-t-ms 20
    python visualize_events.py --input <path> --gif --delta-t-ms 20
    python visualize_events.py --input <path> --gif --denoise-us 20000

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
    parser.add_argument(
        "--denoise-us", type=float, default=0.0,
        help="drop events with no spatially/temporally-correlated neighbor within this many "
        "microseconds (0 = disabled, the default). Try 10000-30000 (10-30ms) as a starting "
        "point; see the module docstring for why this is a DIY filter rather than an SDK one.",
    )
    parser.add_argument(
        "--denoise-radius", type=int, default=1,
        help="neighborhood radius in pixels for --denoise-us (1 = 3x3 neighborhood, the default)",
    )
    parser.add_argument(
        "--denoise-min-neighbors", type=int, default=1,
        help="require at least this many DISTINCT recently-active pixels in the neighborhood, "
        "not just one (default 1 = original behavior). Confirmed on real hardware that "
        "min_neighbors=1 isn't selective enough on this sensor's noise: tightening "
        "--denoise-us alone dropped ~48%% of ALL events with no visible drop in background "
        "noise, meaning single-neighbor pairs of noise are about as common as single-neighbor "
        "real motion. Real motion lights up SEVERAL nearby pixels together; try 2 or 3 here.",
    )
    return parser.parse_args()


def _denoise_keep_mask(
    xs: np.ndarray, ys: np.ndarray, ts: np.ndarray, last_active: np.ndarray, threshold_us: float, radius: int,
    min_neighbors: int = 1,
) -> np.ndarray:
    """Spatiotemporal activity filter -- keeps an event only if at least
    `min_neighbors` DISTINCT pixels within `radius` of it (itself included)
    fired within `threshold_us` before it. Real motion lights up several
    nearby pixels together in a short window; isolated noise doesn't have
    that support and gets dropped. `last_active` is a persistent (height,
    width) array of the last-seen event time per pixel (same units as
    `ts`, i.e. device microseconds), carried across calls so the filter has
    memory across the whole recording rather than resetting every delta-t
    chunk.

    min_neighbors=1 (the default) only requires ONE recently-active
    neighbor -- confirmed on real hardware this isn't selective enough for
    this sensor's actual noise character: tightening the time window alone
    (20ms -> 3ms) dropped ~48% of all events with no visible reduction in
    background noise dots, meaning isolated noise pairs pass a
    single-neighbor test about as easily as real motion does. Raising
    min_neighbors to 2 or 3 is a much stronger test, since real motion
    typically lights up several pixels together, not just two.

    Processes events one at a time in chronological order (required for
    correctness: two real, temporally-close events at neighboring pixels
    within the SAME chunk need to be able to support each other) -- fine
    for an offline diagnostic tool on clip-length recordings; if this gets
    too slow on very large recordings, the per-event neighborhood lookup is
    the place to optimize (e.g. vectorize with a shifted-array approach).

    IMPORTANT: `last_active` gets updated for EVERY event, kept or not --
    not just kept ones. Caught this with a synthetic test: updating only on
    keep meant the very FIRST event of any real motion burst had nothing to
    find as "recently active" yet (nothing came before it either), so it
    got dropped, never updated the grid, and the next event in the same
    burst also found nothing -- cascading to the whole burst being dropped.
    Updating unconditionally lets event 2 of a burst find support from
    event 1 regardless of whether event 1 itself was classified as noise,
    which is what actually makes a moving edge's leading events survive.
    """
    height, width = last_active.shape
    keep = np.zeros(xs.shape[0], dtype=bool)
    for i in range(xs.shape[0]):
        xi = int(xs[i])
        yi = int(ys[i])
        ti = ts[i]
        y0, y1 = max(0, yi - radius), min(height, yi + radius + 1)
        x0, x1 = max(0, xi - radius), min(width, xi + radius + 1)
        # Count DISTINCT recently-active pixels in the neighborhood, not
        # total historical events there -- last_active only ever stores the
        # latest fire time per pixel, so a single pixel repeatedly firing
        # can't masquerade as "several neighbors" here.
        active_count = int((last_active[y0:y1, x0:x1] >= ti - threshold_us).sum())
        keep[i] = active_count >= min_neighbors
        last_active[yi, xi] = ti
    return keep


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

    denoise_enabled = args.denoise_us > 0.0
    total_before_denoise = 0
    if denoise_enabled:
        # Very-negative sentinel so no pixel looks "recently active" before
        # it's actually fired even once -- device timestamps start near 0,
        # so an initial value of 0 would falsely count as recent activity.
        last_active = np.full((height, width), -1e15, dtype=np.float64)
        print(f"Denoising: keeping events with >= {args.denoise_min_neighbors} neighbor(s) active "
              f"within {args.denoise_us:.0f}us (radius={args.denoise_radius}px)")

    total_invalid_coords = 0
    for evs in mv_iterator:
        if evs.size > 0:
            # Confirmed on real hardware: a corrupted/flaky recording can
            # decode events with x/y outside the sensor's actual
            # resolution (e.g. y=696 on a 320-tall sensor), alongside the
            # SDK's own "TimeHigh discrepancy" warnings printed straight to
            # the terminal while parsing such a file. Every array index
            # below (denoise, heatmap accumulation, GIF frame) uses x/y as
            # raw indices with no bounds checking, so one bad event used to
            # crash the whole run (or worse, silently write out of bounds)
            # instead of just being dropped like the garbage it is.
            valid_mask = (evs["x"] >= 0) & (evs["x"] < width) & (evs["y"] >= 0) & (evs["y"] < height)
            if not valid_mask.all():
                total_invalid_coords += int((~valid_mask).sum())
                evs = evs[valid_mask]
        total_before_denoise += int(evs.size)
        if denoise_enabled and evs.size > 0:
            keep_mask = _denoise_keep_mask(
                evs["x"], evs["y"], evs["t"].astype(np.float64), last_active, args.denoise_us, args.denoise_radius,
                min_neighbors=args.denoise_min_neighbors,
            )
            evs = evs[keep_mask]
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

    if total_invalid_coords > 0:
        print(f"WARNING: dropped {total_invalid_coords} events with x/y outside the sensor's "
              f"{width}x{height} resolution -- this file's recording looks corrupted/flaky "
              f"(check for 'TimeHigh discrepancy' messages above from the SDK's own decoder). "
              f"Consider re-recording rather than trusting this file.")

    duration_s = len(chunk_counts) * args.delta_t_ms / 1000.0
    print(f"Total events: {total_events} ({total_pos} positive / {total_neg} negative)")
    print(f"Duration: {duration_s:.2f}s, avg rate: {total_events / duration_s if duration_s > 0 else 0:.0f} events/s")
    if denoise_enabled:
        dropped = total_before_denoise - total_events
        pct = (dropped / total_before_denoise * 100.0) if total_before_denoise > 0 else 0.0
        print(f"Denoise: dropped {dropped} of {total_before_denoise} events ({pct:.1f}%)")

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
