"""BMI088 IMU recorder via libiio, mirroring core/modules/imu/imu_device.cpp
(the already-working IIO device/channel/buffer model in the C++ core stack)
instead of talking to the SPI bus directly.

CHANGED after real-hardware testing: originally used the standard IIO
buffered-capture path (iio.Buffer + a hardware "timestamp" channel), same as
the C++ core stack's model. Confirmed on-device this doesn't work on this
deployment: `iio:device0` has no `trigger/` subdirectory at all (buffered
capture needs something -- a hardware IRQ or a software timer -- telling the
driver when to push a sample into the buffer; without one, buffer/enable
succeeds silently but nothing ever arrives, and refill() times out).
`iio-trig-hrtimer` (the usual software-timer fallback) isn't even built for
this kernel, and configfs's IIO trigger path doesn't exist either. Most
likely cause: this BMI088's interrupt line isn't wired up in the device
tree, so the driver never registers a data-ready trigger of its own.

This matches what raw register reads already showed much earlier in this
project (the accelerometer correctly reading ~1g at rest) -- that was always
going through the simple per-channel `raw` sysfs attribute, not the buffer.
So instead of fighting a capture pipeline this driver doesn't support here,
this recorder now polls `raw` directly in a loop. Real tradeoff, not a
free lunch: the "timestamp" channel's attrs are empty outside buffered mode
(confirmed via introspection) -- there's no hardware timestamp available at
all in this mode, only the host's own now_ns() per read, and actual achieved
rate/jitter is bounded by Python loop + sysfs read overhead, not the
sensor's own precise internal clocking the way triggered capture would give.
`ImuSample.host_timestamp_ns` is what actually happened, not a promise the
requested sampling_frequency was hit -- same "measure, don't just trust the
config" approach used for RGB fps_achieved elsewhere in this toolkit.

SECOND finding, from the first real recording with this polling approach:
every sample came back byte-identical (min == max == mean across an entire
10s recording, for both accel and gyro) despite physically tilting/shaking
the sensor -- `raw` was returning a frozen, stale value, not a fresh
on-demand conversion. Confirmed the fix by direct sysfs testing: `raw`
stayed frozen across a physical tilt until `scan_elements/in_accel_*_en`
and `buffer/enable` were both written 1 (matching the original power-state
fix from much earlier in this project, where the same flags were needed to
move the sensor's ACC_PWR_CONF/ACC_PWR_CTRL registers out of suspend) --
after that, `raw` tracked real motion immediately. So `open()` below still
enables each channel and still creates an `iio.Buffer` (equivalent to
writing buffer/enable=1), but ONLY for that wake-up side effect -- it never
calls `.refill()`/`.read()` on it, since that's the part that hangs without
a trigger. The buffer object is kept alive as `self._buffer` so it isn't
garbage-collected mid-recording, which would presumably disable it again.

NOTE(verify-on-device): the Python `iio` module's exact Context/Device
attribute API can vary by libiio version/build -- this was checked against
the real bindings this deployment has, but if that changes, re-verify.

Confirmed on real hardware: `sampling_frequency` is exposed per-CHANNEL on
this BMI088 driver (`accel_x`/`accel_y`/`accel_z` each have their own), not
as a device-level attribute -- `device.attrs` has no `sampling_frequency`
key at all (only `current_timestamp_clock`, `dev_err`, `dev_state`,
`dump_regs`, `mount_matrix`, `part`). `open()` below sets it on every
channel that exposes the attribute rather than assuming a device-level one.
Still meaningful in polling mode: it configures the sensor's own internal
ODR, which bounds how "fresh" each individual `raw` read can be, even though
we're not using a hardware trigger to synchronize with it.
"""
from __future__ import annotations

import threading
import time
from dataclasses import dataclass
from typing import Callable, Optional

from .clock import now_ns


@dataclass
class ImuSample:
    index: int
    x: float
    y: float
    z: float
    host_timestamp_ns: int
    timestamp_source: str  # always "poll" -- see module docstring for why
                            # there's no hardware-timestamped alternative here


class ImuRecorder:
    """Records samples from one IIO IMU device (e.g. the BMI088
    accelerometer or gyroscope -- each exposed as its own IIO device, per
    configs/*.yaml's `imus` section in the C++ core stack) by polling each
    channel's `raw` attribute directly -- see module docstring for why this
    isn't the standard buffered-capture approach.
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
        self._buffer_samples = buffer_samples  # sizes the wake-up buffer only
                                                 # -- its data path is unused,
                                                 # see open()
        self._context = None
        self._device = None
        self._channels: list = []
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
            # Confirmed on real hardware: without this, `raw` reads return a
            # FROZEN value forever (same reading regardless of real motion --
            # caught by comparing two raw reads with a physical tilt in
            # between: identical both times). Enabling the channel is what
            # actually wakes the sensor's ADC into continuous conversion;
            # `raw` alone doesn't trigger a fresh on-demand sample on this
            # driver the way some IIO drivers' `raw` handlers do.
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
            # aborting the whole recording. Other OSErrors (e.g. an
            # invalid/unsupported rate value) stay fatal, since those mean
            # the requested rate itself was rejected, which is worth
            # surfacing loudly rather than silently ignoring.
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

        # Confirmed on real hardware: creating this buffer (equivalently,
        # writing 1 to buffer/enable) is REQUIRED even though we never call
        # .refill()/.read() on it -- without it, `raw` reads are frozen (see
        # the channel.enabled comment above). With it, `raw` reads track
        # real motion. This is purely for that wake-up side effect; the
        # buffer's actual data path (which needs a hardware trigger this
        # deployment doesn't have -- see module docstring) is never used.
        # Kept as self._buffer so it isn't garbage-collected mid-recording,
        # which could disable it again.
        self._buffer = iio.Buffer(self._device, self._buffer_samples, False)

    def close(self) -> None:
        self._buffer = None
        self._device = None
        self._context = None

    def start(self, on_sample: Callable[[ImuSample], None]) -> None:
        if self._device is None:
            self.open()
        self._stop_event.clear()
        self._thread = threading.Thread(target=self._poll_loop, args=(on_sample,), daemon=True)
        self._thread.start()

    def stop(self) -> None:
        self._stop_event.set()
        if self._thread is not None:
            self._thread.join(timeout=5.0)
            self._thread = None

    def _poll_loop(self, on_sample: Callable[[ImuSample], None]) -> None:
        # No hardware trigger available on this deployment (see module
        # docstring) -- best-effort software polling instead of kernel-
        # buffered capture. 100Hz fallback if no sampling_frequency was
        # configured/set; otherwise aim for the configured rate, though the
        # ACTUAL achieved rate is whatever host_timestamp_ns ends up
        # reflecting, not a guarantee.
        poll_interval_s = 1.0 / self._sampling_frequency if self._sampling_frequency > 0 else 0.01
        warned = False
        while not self._stop_event.is_set():
            host_ns = now_ns()
            try:
                x, y, z = (float(channel.attrs["raw"].value) * self._scale for channel in self._channels)
            except Exception as exc:
                if not warned:
                    # Rate-limit to one warning per recorder instance --
                    # a transient sysfs read glitch shouldn't spam the
                    # console every poll interval if it keeps happening.
                    print(f"WARNING: read error on '{self._device_id}', skipping sample(s) "
                          f"(further errors on this device won't be printed again): {exc}")
                    warned = True
                time.sleep(poll_interval_s)
                continue

            self._sample_count += 1
            on_sample(ImuSample(
                index=self._sample_count,
                x=x, y=y, z=z,
                host_timestamp_ns=host_ns,
                timestamp_source="poll",
            ))
            time.sleep(poll_interval_s)
