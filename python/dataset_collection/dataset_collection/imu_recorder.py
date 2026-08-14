"""BMI088 IMU recorder via libiio, mirroring core/modules/imu/imu_device.cpp
(the already-working IIO device/channel/buffer model in the C++ core stack)
instead of talking to the SPI bus directly.

Per axis-channel sample, the timestamp comes from the IIO driver's hardware
"timestamp" channel when the device exposes one: on Linux that channel is
CLOCK_MONOTONIC already (same clock as clock.now_ns()), so no anchoring is
needed -- it's directly comparable to the Basler/event camera timestamps.
Falls back to one host timestamp per buffer refill (coarser: every sample in
a refill shares that timestamp) if no timestamp channel is found.

NOTE(verify-on-device): the Python `iio` module's exact Context/Device/Buffer
API (attribute names, buffer refill semantics, per-channel read()) can vary
by libiio version/build. Never run against the real SPI/IIO hardware -- check
this against your actual bindings (built from third-party/libiio) first.

Confirmed on real hardware: `sampling_frequency` is exposed per-CHANNEL on
this BMI088 driver (`accel_x`/`accel_y`/`accel_z` each have their own), not
as a device-level attribute -- `device.attrs` has no `sampling_frequency`
key at all (only `current_timestamp_clock`, `dev_err`, `dev_state`,
`dump_regs`, `mount_matrix`, `part`). `open()` below sets it on every
channel that exposes the attribute rather than assuming a device-level one.
"""
from __future__ import annotations

import threading
from dataclasses import dataclass
from typing import Callable, Optional

import numpy as np

from .clock import now_ns


@dataclass
class ImuSample:
    index: int
    x: float
    y: float
    z: float
    host_timestamp_ns: int
    timestamp_source: str  # "hardware" or "buffer_refill" -- see module docstring


class ImuRecorder:
    """Records buffered samples from one IIO IMU device (e.g. the BMI088
    accelerometer or gyroscope -- each exposed as its own IIO device, per
    configs/*.yaml's `imus` section in the C++ core stack).
    """

    def __init__(
        self,
        device_id: str,
        channels: list[str],
        scale: float,
        sampling_frequency: float = 0.0,
        buffer_samples: int = 256,
    ):
        self._device_id = device_id
        self._channel_names = channels
        self._scale = scale
        self._sampling_frequency = sampling_frequency
        self._buffer_samples = buffer_samples
        self._context = None
        self._device = None
        self._channels: list = []
        self._timestamp_channel = None
        self._buffer = None
        self._thread: Optional[threading.Thread] = None
        self._stop_event = threading.Event()
        self._sample_count = 0

    @property
    def sample_count(self) -> int:
        return self._sample_count

    def open(self) -> None:
        import iio

        self._context = iio.Context()
        self._device = self._context.find_device(self._device_id)
        if self._device is None:
            raise RuntimeError(f"IIO device '{self._device_id}' not found")

        self._channels = []
        for name in self._channel_names:
            channel = self._device.find_channel(name)
            if channel is None:
                raise RuntimeError(f"IIO channel '{name}' not found on device '{self._device_id}'")
            channel.enabled = True
            self._channels.append(channel)

        if self._sampling_frequency > 0:
            # Confirmed on real hardware: this driver exposes
            # sampling_frequency per-CHANNEL (accel_x/accel_y/accel_z each
            # have their own), not as a device-level attribute -- the
            # device itself only exposes ['current_timestamp_clock',
            # 'dev_err', 'dev_state', 'dump_regs', 'mount_matrix', 'part'],
            # no 'sampling_frequency' at all. Set it on every channel that
            # exposes the attribute (all axes share one underlying ODR
            # register on this driver, so one would suffice, but setting
            # all of them is harmless and doesn't assume that stays true).
            #
            # PermissionError specifically (confirmed on real hardware) is
            # treated as non-fatal: it means the sysfs attribute exists and
            # we're writing to the right place, but this user lacks write
            # permission on it -- typically a missing udev rule, an
            # environment/setup gap, not a config mistake. Warn and
            # continue at the driver's current/default rate rather than
            # aborting the whole recording, since the actual achieved rate
            # is recoverable from the recorded per-sample
            # host_timestamp_ns anyway (hardware timestamp channel,
            # confirmed available -- see module docstring). Other OSErrors
            # (e.g. an invalid/unsupported rate value) stay fatal, since
            # those mean the requested rate itself was rejected, which is
            # worth surfacing loudly rather than silently ignoring.
            set_on_any = False
            permission_denied = False
            for channel in self._channels:
                if "sampling_frequency" not in channel.attrs:
                    continue
                try:
                    channel.attrs["sampling_frequency"].value = str(self._sampling_frequency)
                    set_on_any = True
                except PermissionError:
                    permission_denied = True
                except OSError as exc:
                    raise RuntimeError(
                        f"Failed to set sampling_frequency={self._sampling_frequency} on "
                        f"channel '{channel.id}' of '{self._device_id}': {exc}"
                    ) from exc
            if not set_on_any and permission_denied:
                print(
                    f"WARNING: no permission to set sampling_frequency={self._sampling_frequency} "
                    f"on '{self._device_id}' (likely a missing udev rule granting write access to "
                    f"the IIO sysfs attributes) -- continuing at the driver's current/default rate "
                    f"instead. Fix the permission if you need this specific rate; the actual "
                    f"achieved rate is still recoverable from the recorded per-sample timestamps."
                )
            elif not set_on_any:
                raise RuntimeError(
                    f"No channel on '{self._device_id}' exposes a 'sampling_frequency' "
                    f"attribute (checked: {[c.id for c in self._channels]}) -- can't set "
                    f"sampling_frequency={self._sampling_frequency}. Pass 0 to skip this and "
                    f"use the driver's default rate instead."
                )

        # Optional hardware timestamp channel -- see module docstring.
        self._timestamp_channel = self._device.find_channel("timestamp")
        if self._timestamp_channel is not None:
            self._timestamp_channel.enabled = True

        self._buffer = iio.Buffer(self._device, self._buffer_samples, False)

    def close(self) -> None:
        self._buffer = None
        self._device = None
        self._context = None

    def start(self, on_sample: Callable[[ImuSample], None]) -> None:
        if self._device is None:
            self.open()
        self._stop_event.clear()
        self._thread = threading.Thread(target=self._read_loop, args=(on_sample,), daemon=True)
        self._thread.start()

    def stop(self) -> None:
        self._stop_event.set()
        if self._thread is not None:
            self._thread.join(timeout=5.0)
            self._thread = None

    def _read_loop(self, on_sample: Callable[[ImuSample], None]) -> None:
        while not self._stop_event.is_set():
            self._buffer.refill()
            refill_host_ns = now_ns()  # fallback timestamp; see below

            axis_data = [
                np.frombuffer(bytes(channel.read(self._buffer)), dtype=np.int16)
                for channel in self._channels
            ]
            num_samples = min(len(axis) for axis in axis_data) if axis_data else 0

            timestamps_ns: Optional[np.ndarray] = None
            source = "buffer_refill"
            if self._timestamp_channel is not None:
                try:
                    raw_ts = np.frombuffer(
                        bytes(self._timestamp_channel.read(self._buffer)), dtype=np.int64
                    )
                    if raw_ts.size >= num_samples:
                        timestamps_ns = raw_ts[:num_samples]
                        source = "hardware"
                except Exception:
                    timestamps_ns = None

            for index in range(num_samples):
                x, y, z = (float(axis_data[axis][index]) * self._scale for axis in range(3))
                host_ns = int(timestamps_ns[index]) if timestamps_ns is not None else refill_host_ns
                self._sample_count += 1
                on_sample(ImuSample(
                    index=self._sample_count,
                    x=x, y=y, z=z,
                    host_timestamp_ns=host_ns,
                    timestamp_source=source,
                ))
