#!/usr/bin/env python3
from __future__ import annotations

import argparse
import os
import sys
from dataclasses import dataclass

import numpy as np

sys.path.insert(0, os.path.abspath("build"))
import core


IMU_AXES = ("x", "y", "z")
IMU_FFT_MIN_MAG_DB = -120.0


@dataclass(frozen=True)
class FftSpectrum:
    frequency_hz: np.ndarray
    magnitude_db: np.ndarray
    sample_rate_hz: float


def estimate_sample_rate_hz(timestamp_ns: np.ndarray) -> float:
    timestamps = np.asarray(timestamp_ns, dtype=np.float64)
    if timestamps.size < 2:
        raise ValueError("Need at least two timestamps to estimate a sample rate")

    deltas_ns = np.diff(timestamps)
    positive_deltas_ns = deltas_ns[deltas_ns > 0.0]
    if positive_deltas_ns.size == 0:
        raise ValueError("Timestamps do not advance monotonically")

    median_period_s = float(np.median(positive_deltas_ns)) / 1_000_000_000.0
    if median_period_s <= 0.0:
        raise ValueError("Estimated sample period must be positive")
    return 1.0 / median_period_s


def compute_fft_spectrum(values: np.ndarray, sample_rate_hz: float) -> FftSpectrum:
    samples = np.asarray(values, dtype=np.float64)
    if samples.size < 2:
        raise ValueError("Need at least two samples to compute an FFT")
    if sample_rate_hz <= 0.0:
        raise ValueError("Sample rate must be positive")

    centered = samples - np.mean(samples)
    window = np.hanning(samples.size)
    if not np.any(window):
        window = np.ones_like(centered)

    windowed = centered * window
    frequency_hz = np.fft.rfftfreq(samples.size, d=1.0 / sample_rate_hz)
    spectrum = np.fft.rfft(windowed)

    normalization = np.sum(window) / 2.0
    if normalization <= 0.0:
        normalization = samples.size / 2.0

    magnitude = np.abs(spectrum) / normalization
    magnitude_db = 20.0 * np.log10(np.maximum(magnitude, 10 ** (IMU_FFT_MIN_MAG_DB / 20.0)))
    return FftSpectrum(frequency_hz=frequency_hz, magnitude_db=magnitude_db, sample_rate_hz=sample_rate_hz)


def imu_topic_fft(series: dict[str, np.ndarray]) -> dict[str, FftSpectrum]:
    sample_rate_hz = estimate_sample_rate_hz(series["timestamp_ns"])
    return {
        axis: compute_fft_spectrum(series[axis], sample_rate_hz)
        for axis in IMU_AXES
    }


def _select_imu_topics(log_file: core.LogFile) -> list[str]:
    topics = sorted(log_file.imu_topics())
    if len(topics) < 2:
        raise ValueError(f"Expected at least two IMU topics, found {len(topics)}")
    if len(topics) > 2:
        print(f"Warning: found {len(topics)} IMU topics, plotting the first two: {topics[:2]}", file=sys.stderr)
    return topics[:2]


def plot_fft(log_file: core.LogFile) -> None:
    import matplotlib.pyplot as plt

    topics = _select_imu_topics(log_file)
    fig, axes = plt.subplots(len(topics), len(IMU_AXES), squeeze=False, figsize=(16, 8))

    for row, topic in enumerate(topics):
        series = log_file.get_imu_data(topic)
        if series is None:
            raise ValueError(f"IMU topic '{topic}' not found in log")

        spectra = imu_topic_fft(series)
        for col, axis_name in enumerate(IMU_AXES):
            spectrum = spectra[axis_name]
            ax = axes[row][col]
            ax.plot(spectrum.frequency_hz, spectrum.magnitude_db)
            ax.set_title(f"{topic} {axis_name} ({spectrum.sample_rate_hz:.1f} Hz)")
            ax.set_xlabel("Frequency [Hz]")
            ax.set_ylabel("Magnitude [dB]")
            ax.set_ylim(IMU_FFT_MIN_MAG_DB, 10.0)
            ax.grid(True)

    fig.suptitle("IMU FFT analysis")
    fig.tight_layout()


def main() -> int:
    parser = argparse.ArgumentParser(description="Plot FFT spectra for the IMU axes in a core MCAP log")
    parser.add_argument("mcap_path", help="Path to the MCAP file")
    args = parser.parse_args()

    log_file = core.read_log_file(args.mcap_path)
    print(log_file.format_metadata())
    plot_fft(log_file)

    import matplotlib.pyplot as plt

    plt.show()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
