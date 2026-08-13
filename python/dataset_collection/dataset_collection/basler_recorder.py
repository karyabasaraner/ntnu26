"""Basler GigE camera recorder using pypylon.

Mirrors the C++ core/modules/camera/pylon_camera.cpp backend: one grab
thread per camera, BayerRG8 -> RGB8 conversion, and frame timestamps derived
from the camera's own GevTimestampTickFrequency clock (not host arrival
time), anchored onto the shared monotonic clock -- see clock.py.

NOTE(verify-on-device): pypylon's exact attribute names (`grab_result.TimeStamp`,
`grab_result.BlockID`, `camera.GevTimestampTickFrequency`) match the
documented pypylon/pylon API as of writing, but were never run against real
hardware -- check these first if something doesn't line up.
"""
from __future__ import annotations

import threading
from dataclasses import dataclass
from typing import Callable, Optional

import numpy as np

from .clock import ClockAnchor, now_ns


@dataclass
class BaslerFrame:
    index: int
    image: np.ndarray  # HxWx3 uint8, RGB
    camera_timestamp_ticks: int
    exposure_host_ns: int  # camera clock, anchored onto the shared monotonic clock -- best
                            # available proxy for "exposure" in Phase 3's latency breakdown;
                            # falls back to raw host arrival time if the tick frequency is unreadable
    arrival_host_ns: int   # host time when RetrieveResult() returned this frame -- "arrival at
                            # Jetson" in Phase 3's latency breakdown


class BaslerRecorder:
    """Records from one Basler camera, selected by serial number (empty
    string selects the first available device -- fine for single-camera
    Phase 1, but callers with multiple cameras must always pass a serial).
    """

    def __init__(
        self,
        serial_number: str = "",
        exposure_time_us: float = 2000.0,
        gain: float = 1.0,
        fps: float = 30.0,
    ):
        self._serial_number = serial_number
        self._exposure_time_us = exposure_time_us
        self._gain = gain
        self._fps = fps
        self._camera = None
        self._converter = None
        self._ns_per_tick = 0.0
        self._clock_anchor = ClockAnchor()
        self._thread: Optional[threading.Thread] = None
        self._stop_event = threading.Event()
        self._frame_count = 0
        self._dropped_count = 0

    @property
    def serial_number(self) -> str:
        return self._serial_number

    @property
    def frame_count(self) -> int:
        return self._frame_count

    @property
    def dropped_count(self) -> int:
        return self._dropped_count

    @property
    def drift_ppm(self) -> float:
        """How far this camera's actual oscillator has turned out to run
        from its nominal GevTimestampTickFrequency, in parts per million --
        see ClockAnchor.drift_from_nominal_ppm. 0.0 until enough frames have
        come in to refine the estimate away from the nominal seed value.
        Recorded per-run in summary.json so drift correction is visible/
        auditable, not just something happening silently in the background.
        """
        return self._clock_anchor.drift_from_nominal_ppm

    def open(self) -> None:
        from pypylon import pylon

        tl_factory = pylon.TlFactory.GetInstance()
        if self._serial_number:
            device_info = pylon.CDeviceInfo()
            device_info.SetSerialNumber(self._serial_number)
            device = tl_factory.CreateDevice(device_info)
        else:
            device = tl_factory.CreateFirstDevice()

        self._camera = pylon.InstantCamera(device)
        self._camera.Open()
        self._serial_number = self._camera.GetDeviceInfo().GetSerialNumber()

        # Confirmed on-device: different physical units of the same nominal
        # "dmA720-290gc" model can support different Bayer CFA variants --
        # one unit only offered BayerGB8, not BayerRG8, crashing here with
        # an AccessException ("Enum entry is not writable", which for a
        # GenICam enum really means "not a valid value for this instance").
        # ImageFormatConverter below correctly demosaics whichever Bayer
        # variant the source actually is, so pick the first available 8-bit
        # Bayer format from THIS camera's own reported options instead of
        # assuming one universally.
        available_formats = list(self._camera.PixelFormat.Symbolics)
        preferred_bayer_order = ["BayerRG8", "BayerGB8", "BayerGR8", "BayerBG8"]
        pixel_format = next((fmt for fmt in preferred_bayer_order if fmt in available_formats), None)
        if pixel_format is None:
            raise RuntimeError(
                f"No supported 8-bit Bayer pixel format found for camera "
                f"{self._serial_number}; available formats: {available_formats}"
            )
        self._camera.PixelFormat.SetValue(pixel_format)
        self._camera.AcquisitionFrameRateEnable.SetValue(True)
        self._camera.AcquisitionFrameRate.SetValue(self._fps)
        self._camera.ExposureAuto.SetValue("Off")
        self._camera.ExposureTime.SetValue(self._exposure_time_us)
        self._camera.GainAuto.SetValue("Off")
        self._camera.Gain.SetValue(self._gain)

        self._converter = pylon.ImageFormatConverter()
        self._converter.OutputPixelFormat = pylon.PixelType_RGB8packed

        # Same approach as the C++ Pylon backend: convert camera ticks to ns
        # using the camera's own reported tick frequency, not an assumption.
        try:
            tick_frequency = self._camera.GevTimestampTickFrequency.GetValue()
            self._ns_per_tick = 1e9 / tick_frequency if tick_frequency > 0 else 0.0
        except Exception:
            self._ns_per_tick = 0.0
        self._clock_anchor = ClockAnchor(device_ticks_to_ns=self._ns_per_tick or 1.0)

    def close(self) -> None:
        if self._camera is not None:
            if self._camera.IsGrabbing():
                self._camera.StopGrabbing()
            if self._camera.IsOpen():
                self._camera.Close()
            self._camera = None

    def start(self, on_frame: Callable[[BaslerFrame], None]) -> None:
        """Starts a background grab thread; on_frame is called for every
        successfully grabbed frame, from that thread -- keep it fast (e.g.
        just enqueue for a separate writer thread/process).
        """
        from pypylon import pylon

        if self._camera is None:
            self.open()
        self._stop_event.clear()
        self._camera.StartGrabbing(pylon.GrabStrategy_LatestImageOnly)
        self._thread = threading.Thread(target=self._grab_loop, args=(on_frame,), daemon=True)
        self._thread.start()

    def stop(self) -> None:
        self._stop_event.set()
        if self._thread is not None:
            self._thread.join(timeout=5.0)
            self._thread = None
        if self._camera is not None and self._camera.IsGrabbing():
            self._camera.StopGrabbing()

    def _grab_loop(self, on_frame: Callable[[BaslerFrame], None]) -> None:
        from pypylon import pylon

        last_block_id: Optional[int] = None
        while not self._stop_event.is_set() and self._camera.IsGrabbing():
            grab_result = self._camera.RetrieveResult(1000, pylon.TimeoutHandling_Return)
            arrival_host_ns = now_ns()  # "arrival at Jetson" -- captured before any conversion work
            if grab_result is None:
                continue
            if not grab_result.GrabSucceeded():
                grab_result.Release()
                continue

            block_id = grab_result.BlockID
            if last_block_id is not None and block_id > last_block_id + 1:
                self._dropped_count += block_id - last_block_id - 1
            last_block_id = block_id

            converted = self._converter.Convert(grab_result)
            image = converted.GetArray()
            camera_ticks = grab_result.TimeStamp
            grab_result.Release()

            exposure_host_ns = (
                # Pass arrival_host_ns as the observed sample for the
                # drift-correcting fit (see ClockAnchor) -- it's already the
                # closest available real-world reading for this tick value,
                # captured immediately after RetrieveResult() returned.
                self._clock_anchor.to_host_ns(camera_ticks, arrival_host_ns)
                if self._ns_per_tick > 0.0
                else arrival_host_ns
            )

            self._frame_count += 1
            on_frame(BaslerFrame(
                index=self._frame_count,
                image=image,
                camera_timestamp_ticks=camera_ticks,
                exposure_host_ns=exposure_host_ns,
                arrival_host_ns=arrival_host_ns,
            ))
