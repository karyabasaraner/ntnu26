import math
import sys
import unittest
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))

import plot_imu_fft


class PlotImuFftTest(unittest.TestCase):
    def test_given_uniform_timestamps_when_sample_rate_is_estimated_then_rate_matches_spacing(self):
        # GIVEN: A uniformly sampled signal at 800 Hz
        timestamps_ns = np.arange(0, 10, dtype=np.float64) * 1_250_000.0

        # WHEN: The sample rate is estimated from timestamps
        sample_rate_hz = plot_imu_fft.estimate_sample_rate_hz(timestamps_ns)

        # THEN: The estimated rate matches the known spacing
        self.assertAlmostEqual(sample_rate_hz, 800.0)

    def test_given_sine_wave_when_fft_is_computed_then_peak_matches_frequency(self):
        # GIVEN: A single-tone signal sampled at 1024 Hz
        sample_rate_hz = 1024.0
        sample_count = 1024
        frequency_hz = 64.0
        time_s = np.arange(sample_count, dtype=np.float64) / sample_rate_hz
        samples = np.sin(2.0 * math.pi * frequency_hz * time_s)

        # WHEN: The FFT spectrum is computed
        spectrum = plot_imu_fft.compute_fft_spectrum(samples, sample_rate_hz)
        peak_index = int(np.argmax(spectrum.magnitude_db))

        # THEN: The dominant peak is at the injected frequency
        self.assertAlmostEqual(spectrum.frequency_hz[peak_index], frequency_hz)

    def test_given_imu_series_when_axis_spectra_are_requested_then_all_axes_are_returned(self):
        # GIVEN: A synthetic IMU series with matching axis arrays
        sample_rate_hz = 400.0
        sample_count = 400
        timestamps_ns = np.arange(sample_count, dtype=np.float64) * (1_000_000_000.0 / sample_rate_hz)
        series = {
            "timestamp_ns": timestamps_ns,
            "x": np.ones(sample_count, dtype=np.float64),
            "y": np.zeros(sample_count, dtype=np.float64),
            "z": np.linspace(-1.0, 1.0, sample_count, dtype=np.float64),
        }

        # WHEN: The per-axis FFT spectra are prepared
        spectra = plot_imu_fft.imu_topic_fft(series)

        # THEN: All three axes are present and share the inferred rate
        self.assertEqual(sorted(spectra.keys()), ["x", "y", "z"])
        for axis_name in ("x", "y", "z"):
            self.assertAlmostEqual(spectra[axis_name].sample_rate_hz, sample_rate_hz)
            self.assertGreater(spectra[axis_name].frequency_hz.size, 0)
            self.assertEqual(spectra[axis_name].frequency_hz.shape, spectra[axis_name].magnitude_db.shape)


if __name__ == "__main__":
    unittest.main()
