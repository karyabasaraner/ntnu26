"""Prophesee event camera recorder using the Metavision SDK Python bindings.

NOTE(verify-on-device): the Metavision Python module exposing the driver
`Camera` class was renamed between SDK releases (`metavision_sdk_driver` in
older SDKs, `metavision_sdk_stream` in newer ones -- check which resolves on
your installed 5.1.1 and drop the fallback below once known). Method names
(`Camera.from_first_available`, `.cd().add_callback`, `.biases().set_from_file`,
`.start_recording`) mirror the documented C++ driver API 1:1 (Prophesee keeps
the Python bindings parallel to C++ on purpose) but were never run against
the real SDK -- this is the same caveat as
core/modules/event_camera/prophesee_event_camera.cpp.
"""
from __future__ import annotations

from pathlib import Path
from typing import Callable, Optional

import numpy as np

from .clock import ClockAnchor
from .event_types import EventBatch

try:
    from metavision_sdk_stream import Camera
except ImportError:
    try:
        from metavision_sdk_driver import Camera  # older SDK releases
    except ImportError as exc:
        raise ImportError(
            "Neither metavision_sdk_stream nor metavision_sdk_driver could be imported. "
            "Install the Metavision SDK (system-wide, not pip) on the Jetson; this cannot "
            "be verified in the dev sandbox this file was written in."
        ) from exc


class EventCameraRecorder:
    """Records from one Prophesee event camera, selected by serial number
    (empty string selects the first available device -- fine for
    single-camera use, but a two-event-camera setup must always pass a
    serial or both instances will race for "first available").
    """

    def __init__(self, serial_number: str = "", bias_file: str = ""):
        self._serial_number = serial_number
        self._bias_file = bias_file
        self._camera: Optional[Camera] = None
        # Metavision device timestamps are microseconds since stream start.
        self._clock_anchor = ClockAnchor(device_ticks_to_ns=1000.0)
        self._event_count = 0
        self._running = False

    @property
    def event_count(self) -> int:
        return self._event_count

    def open(self) -> None:
        self._camera = (
            Camera.from_serial(self._serial_number)
            if self._serial_number
            else Camera.from_first_available()
        )
        if self._bias_file:
            self._camera.biases().set_from_file(self._bias_file)

    def close(self) -> None:
        self._camera = None

    def start_raw_recording(self, output_path: str | Path) -> None:
        """Phase 1 checkpoint: record the sensor's native RAW stream
        verbatim, with no decoding on our side -- the simplest, most robust
        path since it can't drop/misinterpret events locally. The RAW file
        already contains full per-event timestamps in Prophesee's own
        format; see events.raw's companion timestamps.csv in record_event.py
        for what that CSV actually logs (a session log, not per-event data).
        """
        if self._camera is None:
            self.open()
        self._camera.start_recording(str(output_path))
        self._camera.start()
        self._running = True

    def stop_raw_recording(self) -> None:
        if self._camera is not None:
            self._camera.stop()
            self._camera.stop_recording()
        self._running = False

    def stream_events(self, on_events: Callable[[EventBatch], None]) -> None:
        """Phase 4/5: decoded CD events, timestamped in the shared monotonic
        clock domain (device microsecond clock anchored at the first batch),
        for cross-modal alignment with the Basler/IMU recorders. Mirrors
        core/modules/event_camera/prophesee_event_camera.cpp.
        """
        if self._camera is None:
            self.open()

        def _callback(evs) -> None:
            if evs.size == 0:
                return
            # NOTE: per-event Python-level anchoring is O(n) per batch: fine
            # at GenX320-scale event rates, but revisit (vectorize the tick
            # math with numpy directly) if this becomes a bottleneck on a
            # noisier/larger sensor.
            host_ns = np.fromiter(
                (self._clock_anchor.to_host_ns(int(t)) for t in evs["t"]),
                dtype=np.int64,
                count=evs.size,
            )
            self._event_count += evs.size
            on_events(EventBatch(
                x=evs["x"].astype(np.uint16),
                y=evs["y"].astype(np.uint16),
                polarity=evs["p"].astype(np.uint8),
                host_timestamp_ns=host_ns,
            ))

        self._camera.cd().add_callback(_callback)
        self._camera.start()
        self._running = True

    def stop(self) -> None:
        if self._camera is not None and self._running:
            self._camera.stop()
        self._running = False
