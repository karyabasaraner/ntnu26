"""Shared timing utilities.

Every recorder in this toolkit publishes timestamps in one common clock
domain: the process's monotonic clock (`time.monotonic_ns()`, i.e.
CLOCK_MONOTONIC). Device-native clocks (camera ticks, event-camera
microseconds-since-stream-start) are anchored onto this monotonic clock at
the start of acquisition -- mirroring the approach used in the C++ `core`
stack's Pylon/Prophesee backends, so timestamps stay comparable across every
script in this toolkit (and with anything the C++ side later publishes).

Drift correction: a device's real oscillator frequency is never exactly its
nominal/reported value (Phase 3 measured deviations around 0.04%-0.63% on
real hardware, comparing GevTimestampTickFrequency across the 4 RGB
cameras). Converting device ticks to ns using a single fixed nominal rate
means that error accumulates for as
long as the recording runs -- fine for a 5s test clip, not fine for a
multi-minute SLAM recording where it can add up to real misalignment between
sensors. `ClockAnchor` corrects for this by continuously refining its own
ticks-to-ns rate from data observed during the recording itself (a running
least-squares fit, see below), instead of trusting the nominal rate for the
whole recording. The longer the recording runs, the more it converges onto
the device's true rate -- the opposite of the old failure mode, where longer
recordings meant more accumulated error.
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
    monotonic clock, using the first sample seen as the anchor point, and
    self-corrects for oscillator drift as more samples arrive.

    device_ticks_to_ns: multiply a device tick delta by this to get a
    nanosecond delta (e.g. 1e9 / GevTimestampTickFrequency for Basler ticks,
    1000.0 for a device clock already in microseconds like Metavision's).
    This is only the STARTING rate estimate (from the device's nominal/
    reported frequency) -- for a short recording, that's all there is data
    for, and behavior is identical to before. As more (device_ticks,
    observed_host_ns) pairs come in, `to_host_ns` folds each one into a
    running least-squares fit of the *actual* rate, which absorbs the real
    oscillator's deviation from nominal automatically.

    Why least squares instead of e.g. re-anchoring periodically: a periodic
    re-anchor forces a small jump in the output at each re-anchor point
    (the old rate's error suddenly resets). This fit instead keeps the
    ORIGINAL anchor point exact (it's still the first sample, unchanged) and
    only ever refines the slope going forward, so timestamps it has already
    produced are never revised and output never jumps -- it just gets more
    accurate. Per-sample host-arrival jitter (scheduling, GigE/USB latency)
    averages out over the many samples in a real recording rather than
    biasing the estimate, the same way NTP/PTP filter noisy round-trip
    samples down to a clean rate estimate.

    Caveat: this assumes the drift rate itself is roughly constant across
    the recording, true for a crystal oscillator at a stable temperature
    over the timescales this module records at. If very long (multi-hour)
    field recordings ever show the *rate estimate* still visibly changing
    late in a run (log it via current_rate_ns_per_tick), that would mean
    switching to a windowed/recency-weighted fit -- not needed yet.
    """

    def __init__(self, device_ticks_to_ns: float = 1.0):
        self._nominal_ticks_to_ns = device_ticks_to_ns
        self._rate_ns_per_tick = device_ticks_to_ns
        self._device_start_ticks: Optional[int] = None
        self._host_start_ns: Optional[int] = None
        # Running sums for a least-squares fit of host_ns = rate * ticks,
        # forced through the (0, 0) origin in anchor-relative coordinates --
        # i.e. through the anchor point itself, which is why it never causes
        # the anchor's own output to move. O(1) update per sample, no
        # sample history needs to be stored. Kept as Python ints (arbitrary
        # precision) so long recordings with large tick/ns deltas never lose
        # precision from squaring; only the final ratio is computed as a
        # float, which is fine since the ratio itself stays a small,
        # well-conditioned number (ns per tick).
        self._sum_ticks_sq = 0
        self._sum_ticks_ns = 0
        self.sample_count = 0

    def to_host_ns(self, device_ticks: int, observed_host_ns: Optional[int] = None) -> int:
        """Converts one device tick reading to the shared host clock.

        observed_host_ns: the host-side time this tick reading was actually
        observed at (e.g. arrival_host_ns, captured right after the blocking
        grab call returns) -- pass this whenever the caller already has it,
        so the drift fit uses a real, minimally-delayed observation instead
        of an extra now_ns() call made here after the fact. Falls back to
        calling now_ns() itself if omitted.
        """
        if observed_host_ns is None:
            observed_host_ns = now_ns()

        if self._device_start_ticks is None:
            self._device_start_ticks = device_ticks
            self._host_start_ns = observed_host_ns
            self.sample_count = 1
            return observed_host_ns

        assert self._host_start_ns is not None
        delta_ticks = device_ticks - self._device_start_ticks
        delta_host_ns = observed_host_ns - self._host_start_ns

        self._sum_ticks_sq += delta_ticks * delta_ticks
        self._sum_ticks_ns += delta_ticks * delta_host_ns
        self.sample_count += 1
        if self._sum_ticks_sq > 0:
            # Least-squares slope through the origin: minimizes squared
            # error in the *host_ns* estimate across every sample seen so
            # far, in one O(1) update -- this is the drift-corrected rate.
            self._rate_ns_per_tick = self._sum_ticks_ns / self._sum_ticks_sq
        # else: no tick spread yet (e.g. duplicate/frozen ticks) -- keep
        # whatever rate estimate we already have.

        return self._host_start_ns + int(delta_ticks * self._rate_ns_per_tick)

    @property
    def current_rate_ns_per_tick(self) -> float:
        """The current drift-corrected rate estimate (ns per device tick).
        Equals the nominal seed rate until enough samples have been seen to
        refine it."""
        return self._rate_ns_per_tick

    @property
    def drift_from_nominal_ppm(self) -> float:
        """How far the refined rate has diverged from the nominal/reported
        rate it started from, in parts per million. Phase 3 measured
        magnitudes in this ballpark on real hardware (~400ppm on RGB1,
        ~6300ppm on RGB4, i.e. ~0.04% and ~0.63%) by comparing
        GevTimestampTickFrequency across cameras directly; this property
        reports the signed value this fit actually converges to, which is
        not guaranteed to match the sign of that back-of-envelope percent
        (this metric is (fitted_rate - nominal_rate) / nominal_rate: a
        camera whose real tick frequency runs HIGHER than nominal -- i.e.
        its clock is "fast" -- has FEWER real ns per tick than nominal
        assumed, so it converges to a NEGATIVE ppm here). 0.0 until at
        least one real sample has refined the estimate."""
        if self._nominal_ticks_to_ns == 0:
            return 0.0
        return (self._rate_ns_per_tick - self._nominal_ticks_to_ns) / self._nominal_ticks_to_ns * 1e6
