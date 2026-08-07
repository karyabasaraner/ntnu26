"""Shared, SDK-independent event data types.

Split out from event_recorder.py so that code which only needs to know the
*shape* of a decoded event batch (e.g. event_record_io.py's on-disk encoding)
doesn't transitively require the Metavision SDK to be importable.
"""
from __future__ import annotations

from dataclasses import dataclass

import numpy as np


@dataclass
class EventBatch:
    x: np.ndarray               # uint16
    y: np.ndarray                # uint16
    polarity: np.ndarray          # uint8, 0 (OFF) or 1 (ON)
    host_timestamp_ns: np.ndarray  # int64, per-event, shared monotonic clock domain
