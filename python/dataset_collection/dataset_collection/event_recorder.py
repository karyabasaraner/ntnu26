"""Prophesee event camera recorder using the Metavision SDK Python bindings.

Verified on-device against the installed SDK (metavision_sdk_stream resolves
directly, no fallback needed). The originally-assumed API
(`.cd().add_callback`, `.biases().set_from_file`, `.start_recording`) does
NOT exist on this SDK's `Camera` class -- it turned out to be a thin wrapper
exposing only from_serial/from_first_available/from_file/get_device/save/
load/height/width. The real streaming/recording controls live one level
down, on the HAL `Device` object (`Camera.get_device()`), via its
`get_i_*()` facility getters:

  - Raw recording (Phase 1, start_raw_recording/stop_raw_recording below):
    `device.get_i_events_stream()` -> `I_EventsStream` with
    `log_raw_data(path: str) -> bool`, `start()`, `stop()`,
    `stop_log_raw_data()`. Confirmed order: log_raw_data() BEFORE start(),
    stop() BEFORE stop_log_raw_data().

NOTE(verify-on-device): still unverified --
  - Bias file loading: `.biases().set_from_file(...)` doesn't exist either;
    the replacement is presumably `device.get_i_ll_biases()`, but its exact
    method names haven't been checked. `open()` below raises a clear error
    instead of guessing, if a bias_file is actually passed.
  - Decoded CD event streaming (`stream_events()`, used by Phase 4/5):
    `.cd().add_callback(...)` doesn't exist either. The real path is likely
    `device.get_i_event_cd_decoder()` plus a callback registered on that
    decoder, fed by `get_i_events_stream()`'s buffers -- same style of fix
    as start_raw_recording below, just not yet worked through on real
    hardware. `stream_events()` raises a clear error instead of guessing.
"""
from __future__ import annotations

from pathlib import Path
from typing import Callable, Optional

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
        self._i_events_stream = None
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
        self._i_events_stream = self._camera.get_device().get_i_events_stream()
        if self._bias_file:
            # See module NOTE(verify-on-device): device.get_i_ll_biases() is
            # presumably the replacement for the nonexistent .biases(), but
            # its method names haven't been confirmed on real hardware yet.
            raise NotImplementedError(
                "bias_file loading not yet verified against this SDK's "
                "device.get_i_ll_biases() API -- rerun without --bias-file "
                "for now, or verify get_i_ll_biases()'s methods on-device "
                "and implement this before relying on it."
            )

    def close(self) -> None:
        self._camera = None
        self._i_events_stream = None

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
        # Confirmed order on-device: log_raw_data() must be called before
        # start(), so the file captures from the very first event.
        self._i_events_stream.log_raw_data(str(output_path))
        self._i_events_stream.start()
        self._running = True

    def stop_raw_recording(self) -> None:
        if self._i_events_stream is not None:
            self._i_events_stream.stop()
            self._i_events_stream.stop_log_raw_data()
        self._running = False

    def stream_events(self, on_events: Callable[[EventBatch], None]) -> None:
        """Phase 4/5: decoded CD events, timestamped in the shared monotonic
        clock domain (device microsecond clock anchored at the first batch),
        for cross-modal alignment with the Basler/IMU recorders. Mirrors
        core/modules/event_camera/prophesee_event_camera.cpp.

        NOT YET WORKING: see module NOTE(verify-on-device). `.cd()` doesn't
        exist on this SDK's Camera (confirmed on-device alongside the
        start_raw_recording fix). The real path is presumably
        device.get_i_event_cd_decoder() plus a callback fed by
        get_i_events_stream()'s buffers, but that hasn't been worked through
        against real hardware yet -- only Phase 1 raw recording has.
        """
        raise NotImplementedError(
            "stream_events() (decoded CD events, used by Phase 4/5) still "
            "uses the same wrong API guess as the now-fixed raw recording "
            "path -- needs the same on-device verification against "
            "device.get_i_event_cd_decoder() before this can work. "
            "record_event.py's raw recording (Phase 1) works; "
            "record_rgb_event.py / record.py do not yet."
        )
        # Left unimplemented rather than guessed again: whatever replaces
        # this needs to turn decoded (x, y, polarity, t) arrays from
        # get_i_event_cd_decoder()'s callback into EventBatch objects via
        # on_events(), anchoring t (device microseconds) through
        # self._clock_anchor.to_host_ns() same as before, updating
        # self._event_count, and setting self._running = True once the
        # underlying get_i_events_stream() is started.

    def stop(self) -> None:
        if self._i_events_stream is not None and self._running:
            self._i_events_stream.stop()
        self._running = False
