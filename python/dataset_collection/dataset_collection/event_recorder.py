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
    IMPORTANT, also confirmed on-device: log_raw_data() does NOT write to
    disk on its own in the background. Nothing lands in the file unless
    something actively pulls buffers via `get_latest_raw_data()` (which
    returns a `metavision_hal.RawBuffer` -- has `.size()`, not `len()`)
    while recording is active; a plain start()-then-wait produced a fixed
    ~248-byte header-only file regardless of real motion/events at the
    sensor, while the same window with active polling produced ~180KB.
    So start_raw_recording() below runs a background thread pulling
    buffers for the whole recording duration, not just start()/stop().

  - Decoded CD event streaming (Phase 4/5, stream_events() below):
    `.cd().add_callback(...)` doesn't exist, as guessed originally. Confirmed
    on-device: `device.get_i_event_cd_decoder()` DOES exist (a real
    facility getter), but it turned out unnecessary -- `EventsIterator`
    (`metavision_core.event_io`, already used to decode RAW files in
    visualize_events.py) streams LIVE directly when given a serial number
    as `input_path` instead of a file path, producing the exact same
    decoded (x, y, p, t) structured arrays either way. Confirmed on real
    hardware: 3.37M events decoded live over 5s (~675K events/sec). This is
    a much smaller, more robust implementation than hand-wiring HAL
    callbacks, so that's what stream_events() below actually uses.
    IMPORTANT: EventsIterator opens its OWN connection to the sensor from
    the serial string -- stream_events() must NOT also call self.open()
    (which creates self._camera via Camera.from_serial()), or that's two
    simultaneous connections to the same physical device, the same
    "VIDIOC_REQBUFS ... Device or resource busy" conflict seen elsewhere
    in this project when something else already held the camera open.

NOTE(verify-on-device): still unverified --
  - Bias file loading: `.biases().set_from_file(...)` doesn't exist either;
    the replacement is presumably `device.get_i_ll_biases()`, but its exact
    method names haven't been checked. `open()` below raises a clear error
    instead of guessing, if a bias_file is actually passed.
"""
from __future__ import annotations

import threading
import time
from pathlib import Path
from typing import Callable, Optional

import numpy as np

from .clock import ClockAnchor, now_ns
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
        # Raw-recording poll thread (see module note): log_raw_data() only
        # actually writes bytes when something pulls buffers, so we have to
        # keep pulling for the whole recording, not just start()/stop().
        self._poll_thread: Optional[threading.Thread] = None
        self._stop_poll = threading.Event()
        # Decoded-event streaming (Phase 4/5, stream_events() below) runs
        # its own EventsIterator-owned live connection, entirely separate
        # from self._camera/self._i_events_stream above -- see module
        # docstring for why the two paths must never both be open at once.
        self._stream_thread: Optional[threading.Thread] = None
        self._stop_stream = threading.Event()
        self._stream_width = 0
        self._stream_height = 0

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
        self._stop_poll.clear()
        self._poll_thread = threading.Thread(target=self._poll_raw_data_loop, daemon=True)
        self._poll_thread.start()
        self._running = True

    def _poll_raw_data_loop(self) -> None:
        # Confirmed on-device: log_raw_data() does nothing on its own --
        # this loop is what actually drives bytes into the file. ~50Hz was
        # chosen empirically (untested how far the poll interval can be
        # stretched before the sensor's internal buffer -- capped around
        # 32768 bytes in testing -- starts dropping data under heavy motion;
        # revisit if recordings show gaps under high event rates).
        while not self._stop_poll.is_set():
            self._i_events_stream.get_latest_raw_data()
            time.sleep(0.02)

    def stop_raw_recording(self) -> None:
        self._stop_poll.set()
        if self._poll_thread is not None:
            self._poll_thread.join(timeout=2.0)
            self._poll_thread = None
        if self._i_events_stream is not None:
            self._i_events_stream.stop()
            self._i_events_stream.stop_log_raw_data()
        self._running = False

    def stream_events(self, on_events: Callable[[EventBatch], None]) -> None:
        """Phase 4/5: decoded CD events, timestamped in the shared monotonic
        clock domain, for cross-modal alignment with the Basler/IMU
        recorders. Mirrors core/modules/event_camera/prophesee_event_camera.cpp.

        Confirmed on real hardware: metavision_core.event_io.EventsIterator
        (already used to decode RAW files in visualize_events.py) streams
        LIVE directly when given a serial number as input_path, producing
        the same decoded (x, y, p, t) structured arrays -- see module
        docstring. Runs the live iteration in a background thread (daemon,
        same pattern as the raw-recording poll thread) so this call returns
        immediately; on_events() is invoked from that thread once per
        delta_t chunk, so keep it fast (enqueue for a writer, as
        MultiEventRig does).
        """
        if self._bias_file:
            # Same unresolved gap as open()'s raw-recording path -- see
            # module NOTE(verify-on-device).
            raise NotImplementedError(
                "bias_file loading not yet verified against this SDK's "
                "device.get_i_ll_biases() API -- rerun without --bias-file "
                "for now."
            )
        if not self._serial_number:
            # Unlike raw recording (Camera.from_first_available()), a live
            # EventsIterator needs a concrete device to connect to -- no
            # "first available" fallback here, and guessing wrong would
            # silently grab the wrong physical camera in a two-event-camera
            # setup.
            raise ValueError(
                "stream_events() requires an explicit serial_number (no "
                "'first available' fallback for live decoded streaming)."
            )

        from metavision_core.event_io import EventsIterator

        mv_iterator = EventsIterator(input_path=self._serial_number, delta_t=20000)
        self._stream_height, self._stream_width = mv_iterator.get_size()

        self._stop_stream.clear()
        self._stream_thread = threading.Thread(
            target=self._stream_loop, args=(mv_iterator, on_events), daemon=True,
        )
        self._stream_thread.start()
        self._running = True

    def _stream_loop(self, mv_iterator, on_events: Callable[[EventBatch], None]) -> None:
        mv_iter = iter(mv_iterator)
        while not self._stop_stream.is_set():
            try:
                evs = next(mv_iter)
            except StopIteration:
                break
            except AssertionError as exc:
                # Confirmed on real hardware (visualize_events.py hit the
                # identical thing decoding a corrupted RAW file): the SDK's
                # own decoder can raise a bare AssertionError if it sees
                # timestamps go backward. Stop this thread cleanly instead
                # of letting the exception vanish silently in the
                # background -- self._running flips to False either way,
                # same signal a caller would get from a normal stop().
                print(f"WARNING: event stream decoder hit a data inconsistency and stopped "
                      f"({exc}). Recording ended early.")
                break
            if evs.size == 0:
                continue

            # Confirmed on real hardware (same issue hit in
            # visualize_events.py): a flaky moment in the stream can
            # produce x/y outside the sensor's actual resolution -- drop
            # those rather than writing garbage coordinates into the
            # dataset.
            valid_mask = (
                (evs["x"] >= 0) & (evs["x"] < self._stream_width)
                & (evs["y"] >= 0) & (evs["y"] < self._stream_height)
            )
            if not valid_mask.all():
                evs = evs[valid_mask]
                if evs.size == 0:
                    continue

            # Anchor ONCE per chunk, not per event -- confirmed on real
            # hardware this sensor produces ~675K events/sec, so calling
            # ClockAnchor.to_host_ns()'s stateful per-sample fit for every
            # single event would never keep up. Use the chunk's last event
            # as the reference tick, get one drift-corrected host_ns for
            # it, then vectorize every other event's host_ns via the
            # CURRENT rate estimate -- accurate enough since delta_t chunks
            # are only ~20ms, far shorter than the timescale drift
            # correction actually cares about.
            host_now_ns = now_ns()
            reference_t = float(evs["t"][-1])
            anchor_host_ns = self._clock_anchor.to_host_ns(int(round(reference_t)), host_now_ns)
            rate = self._clock_anchor.current_rate_ns_per_tick
            host_ts = anchor_host_ns + (evs["t"].astype(np.float64) - reference_t) * rate

            batch = EventBatch(
                x=evs["x"].copy(),
                y=evs["y"].copy(),
                polarity=evs["p"].copy(),
                host_timestamp_ns=host_ts.astype(np.int64),
            )
            on_events(batch)
            self._event_count += int(evs.size)

        self._running = False

    def stop(self) -> None:
        self._stop_stream.set()
        if self._stream_thread is not None:
            self._stream_thread.join(timeout=5.0)
            self._stream_thread = None
        if self._i_events_stream is not None and self._running:
            self._i_events_stream.stop()
        self._running = False
