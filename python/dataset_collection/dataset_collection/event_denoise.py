"""Shared spatiotemporal activity filter for decoded event-camera data.

Split out of visualize_events.py so visualize_dataset.py (which reads
ALREADY-DECODED EventRecord binaries, see event_record_io.py -- no
Metavision SDK involved at all) can reuse the exact same tuned filter
without pulling in visualize_events.py's unconditional
`from metavision_core.event_io import EventsIterator` import, which would
require the SDK to be installed just to import this function.
"""
from __future__ import annotations

import numpy as np


def denoise_keep_mask(
    xs: np.ndarray, ys: np.ndarray, ts: np.ndarray, last_active: np.ndarray, threshold_us: float, radius: int,
    min_neighbors: int = 1,
) -> np.ndarray:
    """Spatiotemporal activity filter -- keeps an event only if at least
    `min_neighbors` DISTINCT pixels within `radius` of it (itself included)
    fired within `threshold_us` before it. Real motion lights up several
    nearby pixels together in a short window; isolated noise doesn't have
    that support and gets dropped. `last_active` is a persistent (height,
    width) array of the last-seen event time per pixel (same units as
    `ts`), carried across calls so the filter has memory across the whole
    recording rather than resetting every delta-t chunk.

    min_neighbors=1 (the default) only requires ONE recently-active
    neighbor -- confirmed on real hardware this isn't selective enough for
    this sensor's actual noise character at a tight radius: tightening the
    time window alone (20ms -> 3ms) at radius=1 dropped ~48% of all events
    with no visible reduction in background noise dots, meaning isolated
    noise pairs pass a single-neighbor test about as easily as real motion
    does. A wide radius (confirmed better empirically -- see
    visualize_events.py's tuned defaults) makes min_neighbors=1 work well
    again, since the neighborhood itself then provides enough spatial
    context; raise min_neighbors instead for stricter filtering at a
    tighter radius.

    Processes events one at a time in chronological order (required for
    correctness: two real, temporally-close events at neighboring pixels
    within the SAME chunk need to be able to support each other) -- fine
    for offline/diagnostic use on clip-length recordings; if this gets too
    slow on very large recordings, the per-event neighborhood lookup is the
    place to optimize (e.g. vectorize with a shifted-array approach).

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
