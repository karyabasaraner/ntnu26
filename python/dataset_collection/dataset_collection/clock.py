"""Shared timing utilities.

Every recorder in this toolkit publishes timestamps in one common clock
domain: the process's monotonic clock (`time.monotonic_ns()`, i.e.
CLOCK_MONOTONIC). Device-native clocks (camera ticks, event-camera
microseconds-since-stream-start) are anchored onto this monotonic clock once,
at the start of acquisition -- mirroring the approach used in the C++ `core`
stack's Pylon/Prophesee backends, so timestamps stay comparable across every
script in this toolkit (and with anything the C++ side later publishes).

This does NOT correct for clock drift between a sensor's own oscillator and
the host over a long recording -- only single-point anchoring at stream
start. See analyze_latency.py / Phase 3 for checking whether that matters in
practice.
"""
from __future__ import annotations

import time
from typing import Optional


def now_ns() -> int:
    """Monotonic host time in nanoseconds -- the shared clock domain for this
    toolkit. Comparable across processes/recorders as long as they're on the
    same machine and didn't span a reboot.
    """
    return time.monotonic_ns()


def now_wall_iso() -> str:
    """Wall-clock time as a human-readable ISO-ish string, for logs/filenames
    only. Never use this for latency math -- it can jump (NTP sync, DST);
    now_ns() is what all recorders key their data on.
    """
    fractional_us = int((time.time() % 1) * 1_000_000)
    return time.strftime("%Y-%m-%dT%H:%M:%S", time.localtime()) + f".{fractional_us:06d}"


class ClockAnchor:
    """Anchors a device-native clock (integer ticks) onto the shared
    monotonic clock, using the first sample seen as the anchor point.

    device_ticks_to_ns: multiply a device tick delta by this to get a
    nanosecond delta (e.g. 1e9 / GevTimestampTickFrequency for Basler ticks,
    1000.0 for a device clock already in microseconds like Metavision's).
    """

    def __init__(self, device_ticks_to_ns: float = 1.0):
        self._device_ticks_to_ns = device_ticks_to_ns
        self._device_start_ticks: Optional[int] = None
        self._host_start_ns: Optional[int] = None

    def to_host_ns(self, device_ticks: int) -> int:
        if self._device_start_ticks is None:
            self._device_start_ticks = device_ticks
            self._host_start_ns = now_ns()
        delta_ticks = device_ticks - self._device_start_ticks
        assert self._host_start_ns is not None
        return self._host_start_ns + int(delta_ticks * self._device_ticks_to_ns)
