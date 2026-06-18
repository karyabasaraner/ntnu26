import datetime as _datetime
import tempfile
import struct
import unittest
from pathlib import Path
import sys
from unittest import mock

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))

import web_collector
from web_collector import (
    DEFAULT_SHARED_MEMORY_PATH,
    _SHARED_MEMORY_BUFFER_HEADER_SIZE,
    _SHARED_MEMORY_BUFFER_FRAMES_OFFSET,
    _SHARED_MEMORY_DATA_FRAME_OFFSET,
    _SHARED_MEMORY_LAYOUT_BUFFERS_OFFSET,
    _SharedMemoryProbe,
    DataCollectorState,
    _core_binary_path,
    next_log_path,
    parse_config_layout,
    rolling_stats,
)


class WebCollectorHelpersTest(unittest.TestCase):
    def test_given_four_camera_config_when_layout_is_parsed_then_sensor_names_are_returned(self):
        # GIVEN: A minimal four-camera root config
        config = {
            "cameras": [
                {"name": "front_left", "writer": {"width": 1280, "height": 720}},
                {"name": "front_right", "writer": {"width": 1280, "height": 720}},
                {"name": "left", "writer": {"width": 1280, "height": 720}},
                {"name": "right", "writer": {"width": 1280, "height": 720}},
            ],
            "imus": [
                {"name": "accelerometer"},
                {"name": "gyroscope"},
            ],
        }

        # WHEN: The layout is extracted
        layout = parse_config_layout(config)

        # THEN: The camera and IMU names are preserved in order
        self.assertEqual([camera.name for camera in layout.cameras], ["front_left", "front_right", "left", "right"])
        self.assertEqual(layout.imus, ["accelerometer", "gyroscope"])

    def test_given_values_when_rolling_stats_are_requested_then_mean_and_std_are_returned(self):
        # GIVEN: A deterministic set of dt values
        values = [0.01, 0.02, 0.03, 0.04]

        # WHEN: Summary statistics are computed
        stats = rolling_stats(values)

        # THEN: The mean and standard deviation match the expected values
        self.assertAlmostEqual(stats["mean"], 0.025)
        self.assertAlmostEqual(stats["std"], 0.011180339887498949)
        self.assertEqual(stats["samples"], 4)

    def test_given_existing_logs_when_next_log_path_is_requested_then_next_counter_is_used(self):
        # GIVEN: A log directory with existing files for the same day
        with tempfile.TemporaryDirectory() as temp_dir:
            log_dir = Path(temp_dir)
            for name in ["2026-06-18-000.mcap", "2026-06-18-002.mcap", "2026-06-17-999.mcap", "ignore.txt"]:
                (log_dir / name).write_text("", encoding="utf-8")

            # WHEN: The next log path is computed
            path = next_log_path(log_dir, _datetime.date(2026, 6, 18))

            # THEN: The next available counter for the current date is selected
            self.assertEqual(path.name, "2026-06-18-003.mcap")

    def test_given_no_matching_logs_when_next_log_path_is_requested_then_counter_starts_at_zero(self):
        # GIVEN: An empty log directory
        with tempfile.TemporaryDirectory() as temp_dir:
            log_dir = Path(temp_dir)

            # WHEN: The next log path is computed
            path = next_log_path(log_dir, _datetime.date(2026, 6, 18))

            # THEN: The first file for the day uses the zero counter
            self.assertEqual(path.name, "2026-06-18-000.mcap")

    def test_given_shared_memory_file_when_probe_matches_layout_then_ready_is_reported(self):
        # GIVEN: A temporary shared-memory image with the expected buffer names
        with tempfile.TemporaryDirectory() as temp_dir:
            shm_path = Path(temp_dir) / DEFAULT_SHARED_MEMORY_PATH.name
            self._write_shared_memory_image(shm_path, ["front_left", "front_right"])
            probe = _SharedMemoryProbe(["front_left", "front_right"], shm_path)

            # WHEN: The probe checks the shared-memory layout
            ready = probe.is_ready()

            # THEN: The layout is accepted as ready
            self.assertTrue(ready)

    def test_given_missing_shared_memory_when_probe_checks_then_not_ready_is_reported(self):
        # GIVEN: A shared-memory path that does not exist
        with tempfile.TemporaryDirectory() as temp_dir:
            shm_path = Path(temp_dir) / DEFAULT_SHARED_MEMORY_PATH.name
            probe = _SharedMemoryProbe(["front_left"], shm_path)

            # WHEN: The probe checks readiness
            ready = probe.is_ready()

            # THEN: Readiness is rejected
            self.assertFalse(ready)

    def test_given_no_shared_memory_when_state_is_constructed_then_readers_are_not_opened(self):
        # GIVEN: A valid config and a fake core module that would fail if called too early
        with tempfile.TemporaryDirectory() as temp_dir:
            config_path = Path(temp_dir) / "config.yaml"
            config_path.write_text(
                """
cameras:
  - name: cam0
    writer:
      width: 1
      height: 1
imus:
  - name: imu0
""",
                encoding="utf-8",
            )

            fake_core = mock.Mock()
            fake_core.make_reader.side_effect = AssertionError("reader factory must not run during construction")

            with mock.patch.object(web_collector, "_core_module", return_value=fake_core):
                # WHEN: The controller state is created
                state = DataCollectorState(str(config_path), temp_dir)

            # THEN: No readers were constructed during initialization
            self.assertFalse(state._readers_initialized)
            self.assertTrue(all(sensor._reader is None for sensor in state._camera_states))
            self.assertTrue(all(sensor._reader is None for sensor in state._imu_states))

    def test_given_ready_shared_memory_when_collection_runs_then_readers_are_initialized_lazily(self):
        # GIVEN: A controller state with a fake process layer and a ready shared-memory image
        with tempfile.TemporaryDirectory() as temp_dir:
            config_path = Path(temp_dir) / "config.yaml"
            config_path.write_text(
                """
cameras:
  - name: cam0
    writer:
      width: 1
      height: 1
imus:
  - name: imu0
""",
                encoding="utf-8",
            )
            shm_path = Path(temp_dir) / DEFAULT_SHARED_MEMORY_PATH.name
            self._write_shared_memory_image(shm_path, ["cam0", "imu0"])

            fake_core = _FakeCoreModule()

            with mock.patch.object(web_collector, "_ProcessHandle", _FakeProcessHandle), mock.patch.object(
                web_collector, "_core_module", return_value=fake_core
            ), mock.patch.object(web_collector, "_encode_jpeg_bytes", return_value=b"jpeg-bytes"):
                state = DataCollectorState(str(config_path), temp_dir)
                state._shared_memory_probe = _SharedMemoryProbe(["cam0", "imu0"], shm_path)

                # WHEN: The main app is started and the UI snapshots state
                state.start_main_app(str(config_path))
                state._collect_once()
                state._collect_once()
                payload = state.snapshot()

            # THEN: Readers are created only after shared memory is ready
            self.assertTrue(payload["shared_memory_ready"])
            self.assertTrue(all(sensor._reader is not None for sensor in state._camera_states))
            self.assertTrue(all(sensor._reader is not None for sensor in state._imu_states))
            self.assertEqual([call[1] for call in fake_core.calls], ["cam0", "imu0"])
            self.assertFalse(payload["fatal_error"])
            self.assertEqual(payload["dt"]["cameras"]["samples"], 1)
            self.assertEqual(payload["dt"]["imus"]["samples"], 1)
            self.assertEqual(state.camera_frame("cam0"), b"jpeg-bytes")
            self.assertNotIn("image", payload["cameras"][0])

    def test_given_imu_samples_spanning_more_than_ten_seconds_when_history_is_snapshotted_then_old_samples_are_trimmed(self):
        # GIVEN: A scripted IMU reader with timestamps that cross the 10 second window
        reader = _ScriptedReader(
            [
                ("imu0", 0, 1, 1_000_000_000, np.frombuffer(struct.pack("<fff", 1.0, 2.0, 3.0), dtype=np.uint8)),
                ("imu0", 0, 2, 5_000_000_000, np.frombuffer(struct.pack("<fff", 4.0, 5.0, 6.0), dtype=np.uint8)),
                ("imu0", 0, 3, 11_000_000_000, np.frombuffer(struct.pack("<fff", 7.0, 8.0, 9.0), dtype=np.uint8)),
                ("imu0", 0, 4, 12_000_000_000, np.frombuffer(struct.pack("<fff", 10.0, 11.0, 12.0), dtype=np.uint8)),
            ]
        )
        state = web_collector._SensorState("imu0", "imu", reader, history_seconds=10.0)

        # WHEN: The reader advances through the scripted samples
        self.assertTrue(state.update())
        self.assertTrue(state.update())
        self.assertTrue(state.update())
        self.assertTrue(state.update())
        history = state.snapshot().payload["history"]

        # THEN: Only the last ten seconds of samples remain in the ring buffer
        self.assertEqual(len(history), 3)
        self.assertEqual([entry["timestamp_ns"] for entry in history], [5_000_000_000, 11_000_000_000, 12_000_000_000])
        self.assertEqual([entry["dt_s"] for entry in history], [4.0, 6.0, 1.0])
        self.assertEqual([entry["sample"] for entry in history], [[4.0, 5.0, 6.0], [7.0, 8.0, 9.0], [10.0, 11.0, 12.0]])

    def test_given_main_app_exits_before_shared_memory_is_ready_then_fatal_error_is_reported(self):
        # GIVEN: A controller state using fake processes and no ready shared memory
        with tempfile.TemporaryDirectory() as temp_dir:
            config_path = Path(temp_dir) / "config.yaml"
            config_path.write_text(
                """
cameras:
  - name: cam0
    writer:
      width: 1
      height: 1
imus:
  - name: imu0
""",
                encoding="utf-8",
            )

            fake_core = _FakeCoreModule()

            with mock.patch.object(web_collector, "_ProcessHandle", _FakeProcessHandle), mock.patch.object(
                web_collector, "_core_module", return_value=fake_core
            ):
                state = DataCollectorState(str(config_path), temp_dir)
                state.start_main_app(str(config_path))
                state._main_process._running = False

                # WHEN: The UI snapshots state after the producer has exited
                payload = state.snapshot()

            # THEN: The fatal error is surfaced and readers were never created
            self.assertFalse(payload["shared_memory_ready"])
            self.assertIsNotNone(payload["fatal_error"])
            self.assertIn("exited before shared memory became ready", payload["fatal_error"])
            self.assertFalse(fake_core.calls)

    def test_given_main_app_is_stopped_intentionally_then_no_fatal_error_is_reported(self):
        # GIVEN: A controller state running with fake processes
        with tempfile.TemporaryDirectory() as temp_dir:
            config_path = Path(temp_dir) / "config.yaml"
            config_path.write_text(
                """
cameras:
  - name: cam0
    writer:
      width: 1
      height: 1
imus:
  - name: imu0
""",
                encoding="utf-8",
            )

            fake_core = _FakeCoreModule()

            with mock.patch.object(web_collector, "_ProcessHandle", _FakeProcessHandle), mock.patch.object(
                web_collector, "_core_module", return_value=fake_core
            ):
                state = DataCollectorState(str(config_path), temp_dir)
                state.start_main_app(str(config_path))
                state.stop_main_app()
                payload = state.snapshot()

            # THEN: A requested stop is not classified as a crash
            self.assertFalse(payload["fatal_error"])
            self.assertFalse(payload["main_app"]["running"])
            self.assertFalse(payload["shared_memory_ready"])
            self.assertEqual(payload["dt"]["cameras"]["samples"], 0)
            self.assertEqual(payload["dt"]["imus"]["samples"], 0)

    def test_given_core_binary_path_when_requested_then_main_executable_is_returned(self):
        # GIVEN: The repository build layout

        # WHEN: The collector resolves the main app executable
        path = _core_binary_path()

        # THEN: The path points at the binary, not the Python extension directory
        self.assertEqual(path.name, "core-main")

    @staticmethod
    def _write_shared_memory_image(path: Path, buffer_names: list[str]) -> None:
        def buffer_block_size(num_frames: int, size_per_frame: int) -> int:
            return _SHARED_MEMORY_BUFFER_FRAMES_OFFSET + num_frames * (_SHARED_MEMORY_DATA_FRAME_OFFSET + size_per_frame)

        size = _SHARED_MEMORY_LAYOUT_BUFFERS_OFFSET + len(buffer_names) * buffer_block_size(1, 4) + _SHARED_MEMORY_BUFFER_HEADER_SIZE
        image = bytearray(size)
        struct.pack_into("<B", image, 0, len(buffer_names))

        offset = _SHARED_MEMORY_LAYOUT_BUFFERS_OFFSET
        for name in buffer_names:
            name_bytes = name.encode("utf-8")[:31]
            packed_name = name_bytes + b"\0" * (32 - len(name_bytes))
            struct.pack_into("<II32sIIQ", image, offset, 0, 0, packed_name, 1, 4, offset)
            offset += buffer_block_size(1, 4)

        path.write_bytes(bytes(image))


class _FakeReader:
    def __init__(self, name: str):
        self._name = name
        self._calls = 0

    def read(self):
        self._calls += 1
        timestamp_ns = 1_000_000_000 + self._calls * 10_000_000
        if self._name == "cam0":
            return self._name, 0, self._calls, timestamp_ns, np.frombuffer(bytes([1, 2, 3]), dtype=np.uint8)
        return self._name, 0, self._calls, timestamp_ns, np.frombuffer(struct.pack("<fff", 1.0, 2.0, 3.0), dtype=np.uint8)


class _FakeCoreModule:
    def __init__(self):
        self.calls: list[tuple[str, str]] = []

    def make_reader(self, config_path: str, name: str):
        self.calls.append((config_path, name))
        return _FakeReader(name)


class _ScriptedReader:
    def __init__(self, entries):
        self._entries = list(entries)
        self._index = 0

    def read(self):
        if self._index >= len(self._entries):
            return self._entries[-1]
        entry = self._entries[self._index]
        self._index += 1
        return entry


class _FakeProcessHandle:
    def __init__(self, command):
        self.command = command
        self._running = False
        self._pid = 12345

    @property
    def running(self):
        return self._running

    @property
    def pid(self):
        return self._pid if self._running else None

    def start(self):
        self._running = True

    def stop(self):
        self._running = False


if __name__ == "__main__":
    unittest.main()
