from __future__ import annotations


import argparse
import ctypes
import datetime as _datetime
import json
import math
import mmap
import os
import re
import subprocess
import sys
import threading
import time
from dataclasses import dataclass
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any
from urllib.parse import parse_qs, urlparse

import numpy as np


DEFAULT_CONFIG_PATH = "configs/basler-dart.yaml"
DEFAULT_LOG_DIR = "/mnt/storage"
DEFAULT_HOST = "10.147.17.18"
DEFAULT_PORT = 8000
DEFAULT_SHARED_MEMORY_PATH = Path("/dev/shm/shared_dict")

_LOG_FILENAME_RE = re.compile(r"^(?P<date>\d{4}-\d{2}-\d{2})-(?P<counter>\d{3,})\.mcap$")

REPO_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO_ROOT / "build"))


@dataclass(slots=True)
class CameraLayout:
    name: str
    width: int
    height: int


@dataclass(slots=True)
class ConfigLayout:
    cameras: list[CameraLayout]
    imus: list[str]


@dataclass(slots=True)
class RunningStats:
    count: int = 0
    mean: float = 0.0
    m2: float = 0.0

    def reset(self) -> None:
        self.count = 0
        self.mean = 0.0
        self.m2 = 0.0

    def add(self, value: float, weight: int = 1) -> None:
        if weight <= 0:
            return

        weighted_count = self.count + weight
        delta = value - self.mean
        self.mean += (delta * weight) / weighted_count
        delta2 = value - self.mean
        self.m2 += delta * delta2 * weight
        self.count = weighted_count

    def snapshot(self) -> dict[str, float | int]:
        if self.count == 0:
            return {
                "mean": 0.0,
                "std": 0.0,
                "mean_hz": 0.0,
                "std_hz": 0.0,
                "samples": 0,
                "rate_hz": 0.0,
            }

        variance = self.m2 / self.count
        rate_hz = self.mean
        return {
            "mean": self.mean,
            "std": math.sqrt(max(0.0, variance)),
            "mean_hz": self.mean,
            "std_hz": math.sqrt(max(0.0, variance)),
            "samples": self.count,
            "rate_hz": rate_hz,
        }


@dataclass(slots=True)
class StreamSnapshot:
    name: str
    ready: bool
    timestamp_ns: int | None
    sequence: int | None
    dt_s: float | None
    payload: dict[str, Any]


def parse_config_layout(config_data: dict[str, Any]) -> ConfigLayout:
    cameras = []
    for camera in config_data.get("cameras", []) or []:
        writer = camera.get("writer", {}) or {}
        cameras.append(
            CameraLayout(
                name=str(camera["name"]),
                width=int(writer["width"]),
                height=int(writer["height"]),
            )
        )

    imus = []
    for imu in config_data.get("imus", []) or []:
        imus.append(str(imu["name"]))

    return ConfigLayout(cameras=cameras, imus=imus)


def load_config_layout(config_path: str) -> ConfigLayout:
    import yaml

    with open(config_path, "r", encoding="utf-8") as handle:
        config_data = yaml.safe_load(handle) or {}
    return parse_config_layout(config_data)


def rolling_stats(values: list[float] | tuple[float, ...]) -> dict[str, float | int]:
    if not values:
        return {"mean": 0.0, "std": 0.0, "samples": 0}

    array = np.asarray(values, dtype=np.float64)
    return {
        "mean": float(np.mean(array)),
        "std": float(np.std(array)),
        "samples": int(array.size),
    }


def next_log_path(log_dir: Path, today: _datetime.date | None = None) -> Path:
    today = today or _datetime.date.today()
    prefix = today.strftime("%Y-%m-%d")
    maximum_counter = -1

    if log_dir.exists() and log_dir.is_dir():
        for entry in log_dir.iterdir():
            if not entry.is_file():
                continue
            match = _LOG_FILENAME_RE.match(entry.name)
            if match is None or match.group("date") != prefix:
                continue
            maximum_counter = max(maximum_counter, int(match.group("counter")))

    return log_dir / f"{prefix}-{maximum_counter + 1:03d}.mcap"


def _encode_jpeg_bytes(image: np.ndarray) -> bytes:
    import cv2

    frame = np.asarray(image, dtype=np.uint8)
    if frame.ndim == 3 and frame.shape[2] == 3:
        frame = frame[:, :, ::-1]

    success, encoded = cv2.imencode(".jpg", frame, [int(cv2.IMWRITE_JPEG_QUALITY), 85])
    if not success:
        raise RuntimeError("Failed to encode camera frame")
    return encoded.tobytes()


def _reshape_camera_frame(array: Any, width: int, height: int) -> np.ndarray:
    frame = np.asarray(array, dtype=np.uint8)
    return frame.reshape(height, width, 3)


def _decode_imu_sample(array: Any) -> list[float]:
    if isinstance(array, (bytes, bytearray, memoryview)):
        raw = bytes(array)
    else:
        raw = np.asarray(array, dtype=np.uint8).tobytes()
    values = np.frombuffer(raw, dtype=np.float32, count=3)
    if values.size != 3:
        raise ValueError("IMU sample does not contain three float32 values")
    return [float(values[0]), float(values[1]), float(values[2])]


def _sequence_rate_hz(
    previous_sequence: int | None,
    previous_timestamp_ns: int | None,
    current_sequence: int,
    current_timestamp_ns: int,
) -> tuple[float | None, int, float | None]:
    if previous_sequence is None or previous_timestamp_ns is None:
        return None, 0, None

    sequence_delta = current_sequence - previous_sequence
    timestamp_delta_ns = current_timestamp_ns - previous_timestamp_ns
    if sequence_delta <= 0 or timestamp_delta_ns <= 0:
        return None, 0, None

    interval_s = timestamp_delta_ns / 1_000_000_000.0
    sample_dt_s = interval_s / float(sequence_delta)
    rate_hz = 1.0 / sample_dt_s if sample_dt_s > 0.0 else None
    return rate_hz, sequence_delta, sample_dt_s


class _SharedMemoryDataFrameHeader(ctypes.Structure):
    _fields_ = [
        ("timestamp_ns", ctypes.c_uint64),
        ("checksum", ctypes.c_uint32),
        ("sequence", ctypes.c_uint32),
    ]


class _SharedMemoryBufferHeader(ctypes.Structure):
    _fields_ = [
        ("head", ctypes.c_uint32),
        ("sequence", ctypes.c_uint32),
        ("name", ctypes.c_char * 32),
        ("num_frames", ctypes.c_uint32),
        ("size_per_frame", ctypes.c_uint32),
        ("offset", ctypes.c_uint64),
    ]


class _SharedMemoryLayoutHeader(ctypes.Structure):
    _fields_ = [
        ("num_buffers", ctypes.c_uint8),
        ("_padding", ctypes.c_uint8 * 7),
        ("buffers", _SharedMemoryBufferHeader * 0),
    ]


_SHARED_MEMORY_BUFFER_HEADER_SIZE = ctypes.sizeof(_SharedMemoryBufferHeader)
_SHARED_MEMORY_BUFFER_ALIGNMENT = ctypes.alignment(_SharedMemoryBufferHeader)
_SHARED_MEMORY_BUFFER_FRAMES_OFFSET = _SHARED_MEMORY_BUFFER_HEADER_SIZE
_SHARED_MEMORY_DATA_FRAME_OFFSET = ctypes.sizeof(_SharedMemoryDataFrameHeader)
_SHARED_MEMORY_LAYOUT_BUFFERS_OFFSET = getattr(_SharedMemoryLayoutHeader, "buffers").offset


def _buffer_block_size(num_frames: int, size_per_frame: int) -> int:
    return _SHARED_MEMORY_BUFFER_FRAMES_OFFSET + num_frames * (_SHARED_MEMORY_DATA_FRAME_OFFSET + size_per_frame)


def _shared_memory_name(header: _SharedMemoryBufferHeader) -> str:
    raw_name = bytes(header.name)
    return raw_name.split(b"\0", 1)[0].decode("utf-8")


class _SharedMemoryProbe:
    def __init__(self, expected_names: list[str], shared_memory_path: Path = DEFAULT_SHARED_MEMORY_PATH):
        self._expected_names = expected_names
        self._shared_memory_path = shared_memory_path

    def is_ready(self) -> bool:
        if not self._shared_memory_path.exists():
            return False

        try:
            with self._shared_memory_path.open("rb") as handle:
                with mmap.mmap(handle.fileno(), 0, access=mmap.ACCESS_READ) as shared_memory:
                    return self._matches_layout(shared_memory)
        except (OSError, ValueError):
            return False

    def _matches_layout(self, shared_memory: mmap.mmap) -> bool:
        if shared_memory.size() < _SHARED_MEMORY_LAYOUT_BUFFERS_OFFSET:
            return False

        layout = _SharedMemoryLayoutHeader.from_buffer_copy(
            shared_memory[: ctypes.sizeof(_SharedMemoryLayoutHeader)]
        )
        if int(layout.num_buffers) != len(self._expected_names):
            return False

        offset = _SHARED_MEMORY_LAYOUT_BUFFERS_OFFSET
        for expected_name in self._expected_names:
            if offset + _SHARED_MEMORY_BUFFER_HEADER_SIZE > shared_memory.size():
                return False

            header = _SharedMemoryBufferHeader.from_buffer_copy(
                shared_memory[offset : offset + _SHARED_MEMORY_BUFFER_HEADER_SIZE]
            )
            if _shared_memory_name(header) != expected_name:
                return False

            offset += _buffer_block_size(int(header.num_frames), int(header.size_per_frame))

        return True


class _SensorState:
    def __init__(
        self,
        name: str,
        kind: str,
        reader: Any,
        width: int | None = None,
        height: int | None = None,
    ):
        self._name = name
        self._kind = kind
        self._reader = reader
        self._width = width
        self._height = height
        self._lock = threading.Lock()
        self._last_timestamp_ns: int | None = None
        self._last_sequence: int | None = None
        self._last_dt_s: float | None = None
        self._last_sample_count: int | None = None
        self._last_rate_hz: float | None = None
        self._last_ready = False
        self._last_payload: dict[str, Any] = {}

    @property
    def dt_s(self) -> float | None:
        with self._lock:
            return self._last_dt_s

    @property
    def sample_count(self) -> int | None:
        with self._lock:
            return self._last_sample_count

    @property
    def rate_hz(self) -> float | None:
        with self._lock:
            return self._last_rate_hz

    @property
    def ready(self) -> bool:
        with self._lock:
            return self._reader is not None and self._last_ready

    def attach_reader(self, reader: Any) -> None:
        with self._lock:
            self._reader = reader

    def reset(self) -> None:
        with self._lock:
            self._reader = None
            self._last_timestamp_ns = None
            self._last_sequence = None
            self._last_dt_s = None
            self._last_sample_count = None
            self._last_rate_hz = None
            self._last_ready = False
            self._last_payload = {}

    def update(self) -> bool:
        with self._lock:
            reader = self._reader
            if reader is None:
                return False

        key, head, sequence, timestamp_ns, array = reader.read()
        if not key or getattr(array, "size", 0) == 0:
            return False

        current_timestamp_ns = int(timestamp_ns)
        current_sequence = int(sequence)

        with self._lock:
            if self._last_sequence is None or self._last_timestamp_ns is None:
                rate_hz = None
                sample_count = None
                dt_s = None
            else:
                rate_hz, sample_count, dt_s = _sequence_rate_hz(
                    self._last_sequence,
                    self._last_timestamp_ns,
                    current_sequence,
                    current_timestamp_ns,
                )

                if current_sequence == self._last_sequence:
                    return False

                if rate_hz is None or sample_count == 0 or dt_s is None:
                    if current_sequence < self._last_sequence or current_timestamp_ns <= self._last_timestamp_ns:
                        self._last_timestamp_ns = current_timestamp_ns
                        self._last_sequence = current_sequence
                        self._last_dt_s = None
                        self._last_sample_count = None
                        self._last_rate_hz = None
                        self._last_ready = True
                    return False

            if self._kind == "camera":
                payload = {
                    "frame": array,
                    "head": int(head),
                }
            else:
                sample = _decode_imu_sample(array)
                payload = {
                    "sample": sample,
                    "head": int(head),
                }

            payload["rate_hz"] = rate_hz
            payload["sample_count"] = sample_count
            payload["dt_s"] = dt_s

            self._last_timestamp_ns = current_timestamp_ns
            self._last_sequence = current_sequence
            self._last_dt_s = dt_s
            self._last_sample_count = sample_count
            self._last_rate_hz = rate_hz
            self._last_ready = True
            self._last_payload = payload
            return True

    def camera_frame_bytes(self) -> bytes | None:
        if self._kind != "camera":
            return None

        with self._lock:
            if not self._last_ready:
                return None

            sequence = self._last_sequence
            cached_sequence = self._last_payload.get("encoded_sequence")
            cached_bytes = self._last_payload.get("encoded_frame_bytes")
            frame = self._last_payload.get("frame")
            width = self._width
            height = self._height

        if sequence is None or frame is None or width is None or height is None:
            return None

        if cached_sequence == sequence and cached_bytes is not None:
            return cached_bytes

        image = _reshape_camera_frame(frame, width, height)
        encoded_bytes = _encode_jpeg_bytes(image)

        with self._lock:
            if self._last_sequence == sequence and self._last_payload.get("frame") is frame:
                self._last_payload["encoded_sequence"] = sequence
                self._last_payload["encoded_frame_bytes"] = encoded_bytes

        return encoded_bytes

    def snapshot(self) -> StreamSnapshot:
        with self._lock:
            return StreamSnapshot(
                self._name,
                self._last_ready,
                self._last_timestamp_ns,
                self._last_sequence,
                self._last_dt_s,
                self._last_payload,
            )


class _ProcessHandle:
    def __init__(self, command: list[str]):
        self._command = command
        self._process: subprocess.Popen[str] | None = None

    @property
    def running(self) -> bool:
        return self._process is not None and self._process.poll() is None

    @property
    def pid(self) -> int | None:
        if self._process is None:
            return None
        return self._process.pid

    def start(self) -> None:
        if self.running:
            raise RuntimeError("process already running")
        binary_path = Path(self._command[0])
        if not binary_path.is_file():
            raise FileNotFoundError(f"Executable not found: {binary_path}")
        if not os.access(binary_path, os.X_OK):
            raise PermissionError(f"Executable is not runnable: {binary_path}")
        self._process = subprocess.Popen(self._command, start_new_session=True)

    def stop(self) -> None:
        if not self.running or self._process is None:
            raise RuntimeError("process is not running")
        self._process.terminate()
        try:
            self._process.wait(timeout=5.0)
        except subprocess.TimeoutExpired:
            self._process.kill()
            self._process.wait(timeout=5.0)


class DataCollectorState:
    def __init__(self, config_path: str, log_dir: str):
        self._lock = threading.RLock()
        self._config_path = config_path
        self._log_dir = log_dir
        self._layout = ConfigLayout(cameras=[], imus=[])
        self._camera_states: list[_SensorState] = []
        self._imu_states: list[_SensorState] = []
        self._shared_memory_probe = _SharedMemoryProbe([])
        self._readers_initialized = False
        self._main_app_started = False
        self._fatal_error: str | None = None
        self._camera_rate_stats = RunningStats()
        self._imu_rate_stats = RunningStats()
        self._main_process = _ProcessHandle([str(_core_binary_path()), config_path])
        self._logger_process = _ProcessHandle([str(_logger_binary_path()), config_path, str(next_log_path(Path(log_dir)))])
        self._current_log_path: Path | None = None
        self._load_layout(config_path)
        self._collector_stop = threading.Event()
        self._collector_thread: threading.Thread | None = None

    def _load_layout(self, config_path: str) -> None:
        layout = load_config_layout(config_path)
        camera_states = [
            _SensorState(camera.name, "camera", None, width=camera.width, height=camera.height)
            for camera in layout.cameras
        ]
        imu_states = [_SensorState(imu_name, "imu", None) for imu_name in layout.imus]

        self._layout = layout
        self._camera_states = camera_states
        self._imu_states = imu_states
        self._shared_memory_probe = _SharedMemoryProbe([camera.name for camera in layout.cameras] + layout.imus)
        self._readers_initialized = False
        self._config_path = config_path

    def start_background_collection(self) -> None:
        with self._lock:
            if self._collector_thread is not None:
                return
            self._collector_thread = threading.Thread(target=self._collector_loop, daemon=True)
            self._collector_thread.start()

    def close(self) -> None:
        self._collector_stop.set()
        if self._collector_thread is not None:
            self._collector_thread.join(timeout=2.0)
            self._collector_thread = None
        self.stop_all()

    def _reset_runtime_state(self) -> None:
        self._readers_initialized = False
        self._camera_rate_stats.reset()
        self._imu_rate_stats.reset()
        for state in self._camera_states:
            state.reset()
        for state in self._imu_states:
            state.reset()

    def _collector_loop(self) -> None:
        while not self._collector_stop.is_set():
            try:
                self._collect_once()
            except Exception:
                pass
            time.sleep(0.001)

    def _collect_once(self) -> None:
        with self._lock:
            if self._fatal_error is not None:
                return

            if self._main_process.running and not self._readers_initialized:
                try:
                    self._ensure_readers_initialized()
                except Exception as error:
                    self._mark_fatal_error(str(error))
                    return

            if not self._readers_initialized:
                return

            for state in self._camera_states:
                if state.update():
                    rate_hz = state.rate_hz
                    sample_count = state.sample_count
                    if rate_hz is not None and sample_count is not None:
                        self._camera_rate_stats.add(rate_hz, sample_count)

            for state in self._imu_states:
                if state.update():
                    rate_hz = state.rate_hz
                    sample_count = state.sample_count
                    if rate_hz is not None and sample_count is not None:
                        self._imu_rate_stats.add(rate_hz, sample_count)

    def _refresh_layout_if_needed(self, config_path: str) -> None:
        if config_path != self._config_path:
            self._load_layout(config_path)

    def _ensure_readers_initialized(self) -> None:
        if self._readers_initialized or self._fatal_error is not None:
            return

        if not self._main_process.running or not self._shared_memory_probe.is_ready():
            return

        core_module = _core_module()
        for state in self._camera_states:
            reader = core_module.make_reader(self._config_path, state._name)
            if reader is None:
                raise RuntimeError(f"shared memory reader unavailable for camera '{state._name}'")
            state.attach_reader(reader)
        for state in self._imu_states:
            reader = core_module.make_reader(self._config_path, state._name)
            if reader is None:
                raise RuntimeError(f"shared memory reader unavailable for IMU '{state._name}'")
            state.attach_reader(reader)

        self._readers_initialized = True

    def _mark_fatal_error(self, message: str) -> None:
        self._fatal_error = message
        self._main_app_started = False
        self._reset_runtime_state()
        if self._logger_process.running:
            try:
                self._logger_process.stop()
            except Exception:
                pass
        if self._main_process.running:
            try:
                self._main_process.stop()
            except Exception:
                pass

    def start_main_app(self, config_path: str) -> dict[str, Any]:
        with self._lock:
            if self._main_process.running:
                raise RuntimeError("main application is already running")
            self._refresh_layout_if_needed(config_path)
            self._fatal_error = None
            self._reset_runtime_state()
            self._main_process = _ProcessHandle([str(_core_binary_path()), config_path])
            self._main_process.start()
            self._main_app_started = True
            return self.snapshot()

    def stop_main_app(self) -> dict[str, Any]:
        with self._lock:
            if self._logger_process.running:
                raise RuntimeError("stop logging before stopping the main application")
            self._main_process.stop()
            self._main_app_started = False
            self._reset_runtime_state()
            return self.snapshot()

    def start_logging(self, config_path: str, log_dir: str) -> dict[str, Any]:
        with self._lock:
            if not self._main_process.running:
                raise RuntimeError("start the main application before logging")
            if self._logger_process.running:
                raise RuntimeError("logging is already running")
            if config_path != self._config_path:
                raise RuntimeError("logging config must match the running application config")
            if not Path(log_dir).is_dir():
                raise RuntimeError(f"log directory does not exist: {log_dir}")
            try:
                self._ensure_readers_initialized()
            except Exception as error:
                self._mark_fatal_error(str(error))
                raise
            if not self._readers_initialized:
                raise RuntimeError("shared memory is not ready yet")
            self._log_dir = log_dir
            output_path = next_log_path(Path(log_dir))
            self._logger_process = _ProcessHandle([str(_logger_binary_path()), config_path, str(output_path)])
            self._logger_process.start()
            self._current_log_path = output_path
            snapshot = self.snapshot()
            snapshot["log_path"] = str(output_path)
            return snapshot

    def stop_logging(self) -> dict[str, Any]:
        with self._lock:
            self._logger_process.stop()
            self._current_log_path = None
            return self.snapshot()

    def stop_all(self) -> None:
        with self._lock:
            if self._logger_process.running:
                self._logger_process.stop()
            if self._main_process.running:
                self._main_process.stop()
            self._current_log_path = None
            self._main_app_started = False
            self._reset_runtime_state()

    def _camera_snapshot(self) -> list[dict[str, Any]]:
        camera_snapshots = []
        for state in self._camera_states:
            snapshot = state.snapshot()
            camera_snapshots.append(
                {
                    "name": snapshot.name,
                    "ready": snapshot.ready,
                    "timestamp_ns": snapshot.timestamp_ns,
                    "sequence": snapshot.sequence,
                    "dt_s": snapshot.dt_s,
                    "rate_hz": snapshot.payload.get("rate_hz"),
                    "sample_count": snapshot.payload.get("sample_count"),
                }
            )

        return camera_snapshots

    def _imu_snapshot(self) -> list[dict[str, Any]]:
        imu_snapshots = []
        for state in self._imu_states:
            snapshot = state.snapshot()
            imu_snapshots.append(
                {
                    "name": snapshot.name,
                    "ready": snapshot.ready,
                    "timestamp_ns": snapshot.timestamp_ns,
                    "sequence": snapshot.sequence,
                    "dt_s": snapshot.dt_s,
                    "rate_hz": snapshot.payload.get("rate_hz"),
                    "sample_count": snapshot.payload.get("sample_count"),
                    "sample": snapshot.payload.get("sample"),
                }
            )

        return imu_snapshots

    def camera_snapshot(self) -> dict[str, Any]:
        with self._lock:
            return {
                "config_path": self._config_path,
                "log_dir": self._log_dir,
                "shared_memory_ready": self._readers_initialized,
                "fatal_error": self._fatal_error,
                "main_app": {
                    "running": self._main_process.running,
                    "pid": self._main_process.pid,
                },
                "logging": {
                    "running": self._logger_process.running,
                    "pid": self._logger_process.pid,
                },
                "cameras": self._camera_snapshot(),
            }

    def camera_frame(self, name: str) -> bytes | None:
        target_state: _SensorState | None = None
        with self._lock:
            for state in self._camera_states:
                if state._name == name:
                    target_state = state
                    break
        if target_state is None:
            return None
        return target_state.camera_frame_bytes()

    def snapshot(self) -> dict[str, Any]:
        with self._lock:
            if self._fatal_error is None and self._main_app_started and not self._main_process.running and not self._readers_initialized:
                self._fatal_error = "main application exited before shared memory became ready"
                if self._logger_process.running:
                    self._logger_process.stop()

            return {
                "config_path": self._config_path,
                "log_dir": self._log_dir,
                "log_path": str(self._current_log_path) if self._current_log_path is not None else (
                    str(next_log_path(Path(self._log_dir))) if Path(self._log_dir).is_dir() else None
                ),
                "shared_memory_ready": self._readers_initialized,
                "fatal_error": self._fatal_error,
                "main_app": {
                    "running": self._main_process.running,
                    "pid": self._main_process.pid,
                },
                "logging": {
                    "running": self._logger_process.running,
                    "pid": self._logger_process.pid,
                },
                "cameras": self._camera_snapshot(),
                "imus": self._imu_snapshot(),
                "dt": {
                    "cameras": self._camera_rate_stats.snapshot(),
                    "imus": self._imu_rate_stats.snapshot(),
                },
            }


def _core_module() -> Any:
    import core

    return core


def _core_binary_path() -> Path:
    return Path(__file__).resolve().parents[1] / "build" / "core-main"


def _logger_binary_path() -> Path:
    return Path(__file__).resolve().parents[1] / "build" / "core-log"


WEB_HTML = """<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>Core Data Collector</title>
  <style>
    :root {
      color-scheme: dark;
      --bg: #0d1117;
      --panel: #11161d;
      --surface: #0b0f14;
      --line: #263041;
      --text: #e6edf3;
      --muted: #9aa4b2;
      --accent: #4fb3ff;
      --good: #3ddc97;
      --bad: #ff6b6b;
    }
    * { box-sizing: border-box; }
    html, body { height: 100%; margin: 0; background: var(--bg); color: var(--text); font: 13px/1.4 system-ui, sans-serif; }
    body { display: grid; grid-template-rows: auto 1fr; }
    header {
      display: grid;
      grid-template-columns: 1fr auto auto;
      gap: 12px;
      align-items: end;
      padding: 12px;
      border-bottom: 1px solid var(--line);
      background: rgba(10, 14, 18, 0.95);
      position: sticky;
      top: 0;
      z-index: 10;
    }
    .field-group { display: grid; gap: 6px; }
    .field-row { display: flex; flex-wrap: wrap; gap: 8px; align-items: center; }
    label { color: var(--muted); font-size: 12px; }
    input {
      min-width: 280px;
      padding: 8px 10px;
      border: 1px solid var(--line);
      background: var(--surface);
      color: var(--text);
      font: inherit;
    }
    input:focus { outline: 1px solid var(--accent); outline-offset: 0; }
    button {
      padding: 8px 10px;
      border: 1px solid var(--line);
      background: #101722;
      color: var(--text);
      font: inherit;
      cursor: pointer;
    }
    button:hover { border-color: var(--accent); }
    button:disabled { opacity: 0.45; cursor: not-allowed; }
    main { min-width: 0; overflow: auto; padding: 12px; display: grid; gap: 12px; }
    .statusbar { display: flex; flex-wrap: wrap; gap: 16px; color: var(--muted); }
    .statusbar strong { color: var(--text); }
    .band {
      display: grid;
      gap: 8px;
      padding: 12px;
      border: 1px solid var(--line);
      background: var(--panel);
    }
    .band h2 { margin: 0; font-size: 12px; text-transform: uppercase; letter-spacing: 0; color: var(--muted); }
    .camera-grid { display: grid; gap: 8px; grid-template-columns: repeat(auto-fit, minmax(320px, 1fr)); }
    .camera-card, .plot-card {
      border: 1px solid var(--line);
      background: var(--surface);
      min-width: 0;
    }
    .camera-card img {
      display: block;
      width: 100%;
      aspect-ratio: 4 / 3;
      object-fit: contain;
      background: #05070a;
    }
    .card-caption { padding: 6px 8px; border-top: 1px solid var(--line); color: var(--muted); white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
    .plot-card canvas { height: 140px; }
    .plot-meta {
      display: flex;
      justify-content: space-between;
      gap: 8px;
      padding: 6px 8px;
      border-top: 1px solid var(--line);
      color: var(--muted);
    }
    .summary-grid { display: grid; gap: 8px; grid-template-columns: repeat(auto-fit, minmax(280px, 1fr)); }
    .summary-box {
      border: 1px solid var(--line);
      background: var(--surface);
      padding: 10px;
    }
    .summary-box strong { color: var(--text); }
    .summary-box .label { color: var(--muted); display: block; margin-bottom: 6px; }
    .summary-list { display: grid; gap: 4px; }
    .summary-line {
      display: flex;
      flex-wrap: wrap;
      gap: 8px;
      justify-content: space-between;
      align-items: baseline;
      color: var(--muted);
    }
    .summary-line strong { color: var(--text); font-weight: 600; }
    .muted { color: var(--muted); }
    .good { color: var(--good); }
    .bad { color: var(--bad); }
    @media (max-width: 900px) {
      header { grid-template-columns: 1fr; }
      input { min-width: 0; width: 100%; }
    }
  </style>
</head>
<body>
  <header>
    <div class="field-group">
      <label for="config-path">Config</label>
      <div class="field-row">
        <input id="config-path" type="text" value="__DEFAULT_CONFIG__">
        <button id="start-app" type="button">Start app</button>
        <button id="stop-app" type="button">Stop app</button>
      </div>
    </div>
    <div class="field-group">
      <label for="log-dir">Log folder</label>
      <div class="field-row">
        <input id="log-dir" type="text" value="__DEFAULT_LOG_DIR__">
        <button id="start-log" type="button">Start logging</button>
        <button id="stop-log" type="button">Stop logging</button>
      </div>
    </div>
    <div class="field-group">
      <label>Log file</label>
      <div class="field-row">
        <div id="log-path" class="muted">-</div>
      </div>
    </div>
  </header>
  <main>
    <div class="statusbar">
      <div>App: <strong id="app-state">stopped</strong></div>
      <div>Logger: <strong id="log-state">stopped</strong></div>
      <div>Config: <strong id="current-config">-</strong></div>
      <div>Log dir: <strong id="current-log-dir">-</strong></div>
    </div>
    <div id="fatal-error" class="muted"></div>

    <section class="band">
      <h2>Camera Views</h2>
      <div id="camera-grid" class="camera-grid"></div>
    </section>

    <section class="band">
      <h2>Dt Summary</h2>
      <div class="summary-grid">
        <div class="summary-box">
          <span class="label">Cameras</span>
          <div id="camera-summary" class="summary-list"></div>
        </div>
        <div class="summary-box">
          <span class="label">IMUs</span>
          <div id="imu-summary" class="summary-list"></div>
        </div>
      </div>
    </section>
  </main>
  <script>
    const configInput = document.getElementById('config-path');
    const logDirInput = document.getElementById('log-dir');
    const startAppButton = document.getElementById('start-app');
    const stopAppButton = document.getElementById('stop-app');
    const startLogButton = document.getElementById('start-log');
    const stopLogButton = document.getElementById('stop-log');
    const logPathElement = document.getElementById('log-path');
    const appStateElement = document.getElementById('app-state');
    const logStateElement = document.getElementById('log-state');
    const currentConfigElement = document.getElementById('current-config');
    const currentLogDirElement = document.getElementById('current-log-dir');
    const fatalErrorElement = document.getElementById('fatal-error');
    const cameraGrid = document.getElementById('camera-grid');
    const cameraSummary = document.getElementById('camera-summary');
    const imuSummary = document.getElementById('imu-summary');
    let cameraCards = new Map();
    let deviceSummaries = {
      cameras: new Map(),
      imus: new Map(),
    };

    class WeightedStats {
      constructor() {
        this.count = 0;
        this.mean = 0;
        this.m2 = 0;
      }

      reset() {
        this.count = 0;
        this.mean = 0;
        this.m2 = 0;
      }

      add(value, weight = 1) {
        if (!Number.isFinite(value) || !Number.isFinite(weight) || weight <= 0) {
          return;
        }

        const weightedCount = this.count + weight;
        const delta = value - this.mean;
        this.mean += (delta * weight) / weightedCount;
        const delta2 = value - this.mean;
        this.m2 += delta * delta2 * weight;
        this.count = weightedCount;
      }

      snapshot() {
        if (this.count <= 0) {
          return { mean: 0, std: 0, samples: 0 };
        }
        return {
          mean: this.mean,
          std: Math.sqrt(Math.max(0, this.m2 / this.count)),
          samples: this.count,
        };
      }
    }

    function ensureCameraCards(cameras) {
      const names = cameras.map((camera) => camera.name).join('|');
      if (cameraGrid.dataset.names === names) {
        return;
      }
      for (const card of cameraCards.values()) {
        if (card.objectUrl) {
          URL.revokeObjectURL(card.objectUrl);
        }
      }
      cameraGrid.dataset.names = names;
      cameraGrid.innerHTML = '';
      cameraCards = new Map();
      for (const camera of cameras) {
        const card = document.createElement('div');
        card.className = 'camera-card';
        const image = document.createElement('img');
        const caption = document.createElement('div');
        caption.className = 'card-caption';
        caption.textContent = camera.name;
        card.append(image, caption);
        cameraGrid.append(card);
        cameraCards.set(camera.name, {
          image,
          caption,
          objectUrl: null,
          endpoint: `/api/camera_frame?name=${encodeURIComponent(camera.name)}`,
          ready: false,
          latestSequence: null,
          renderedSequence: null,
          inFlight: false,
        });
      }
    }

    function formatNumber(value) {
      return Number.isFinite(value) ? value.toFixed(4) : '-';
    }

    function formatSequenceList(streams) {
      if (!Array.isArray(streams) || streams.length === 0) {
        return '-';
      }
      return streams
        .map((stream) => `${stream.name}=${stream.sequence ?? '-'}`)
        .join(', ');
    }

    function ensureSummaryEntries(groupName, streams) {
      const summaries = deviceSummaries[groupName];
      const names = new Set(streams.map((stream) => stream.name));
      for (const name of Array.from(summaries.keys())) {
        if (!names.has(name)) {
          summaries.delete(name);
        }
      }
      for (const stream of streams) {
        if (!summaries.has(stream.name)) {
          summaries.set(stream.name, {
            stats: new WeightedStats(),
            lastSequence: null,
            sequence: null,
            rateHz: null,
            sampleCount: null,
          });
        }
      }
    }

    function updateSummaryState(groupName, streams) {
      ensureSummaryEntries(groupName, streams);
      const summaries = deviceSummaries[groupName];
      for (const stream of streams) {
        const summary = summaries.get(stream.name);
        if (!summary) {
          continue;
        }

        if (summary.sequence !== null && stream.sequence !== null && stream.sequence < summary.sequence) {
          summary.stats.reset();
          summary.lastSequence = null;
        }

        if (stream.sequence !== null && stream.sequence !== summary.lastSequence) {
          if (Number.isFinite(stream.rate_hz) && Number.isFinite(stream.sample_count)) {
            summary.stats.add(stream.rate_hz, stream.sample_count);
          }
          summary.lastSequence = stream.sequence;
        }

        summary.sequence = stream.sequence;
        summary.rateHz = stream.rate_hz;
        summary.sampleCount = stream.sample_count;
      }
    }

    function renderSummaryList(container, streams, groupName) {
      container.innerHTML = '';
      if (!Array.isArray(streams) || streams.length === 0) {
        container.textContent = '-';
        return;
      }

      const summaries = deviceSummaries[groupName];
      for (const stream of streams) {
        const summary = summaries.get(stream.name);
        const stats = summary ? summary.stats.snapshot() : { mean: 0, std: 0 };
        const line = document.createElement('div');
        line.className = 'summary-line';

        const name = document.createElement('strong');
        name.textContent = stream.name;

        const details = document.createElement('span');
        const meanText = summary && summary.stats.count > 0 ? `${formatNumber(stats.mean)} Hz` : '-';
        const stdText = summary && summary.stats.count > 0 ? `${formatNumber(stats.std)} Hz` : '-';
        details.textContent = `mean ${meanText}, std ${stdText}, seq ${stream.sequence ?? '-'}`;

        line.append(name, details);
        container.append(line);
      }
    }

    async function postJson(path, body) {
      const response = await fetch(path, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(body || {})
      });
      const payload = await response.json().catch(() => ({}));
      if (!response.ok) {
        throw new Error(payload.error || `HTTP ${response.status}`);
      }
      return payload;
    }

    async function refreshState() {
      const response = await fetch('/api/state', { cache: 'no-store' });
      const payload = await response.json();

      currentConfigElement.textContent = payload.config_path;
      currentLogDirElement.textContent = payload.log_dir;
      logPathElement.textContent = payload.log_path || '-';
      fatalErrorElement.textContent = payload.fatal_error || '';
      if (payload.fatal_error) {
        appStateElement.textContent = 'fatal';
        appStateElement.className = 'bad';
      } else if (payload.main_app.running && !payload.shared_memory_ready) {
        appStateElement.textContent = 'starting';
        appStateElement.className = 'muted';
      } else {
        appStateElement.textContent = payload.main_app.running ? 'running' : 'stopped';
        appStateElement.className = payload.main_app.running ? 'good' : 'bad';
      }
      logStateElement.textContent = payload.logging.running ? 'running' : 'stopped';
      logStateElement.className = payload.logging.running ? 'good' : 'bad';
      updateSummaryState('cameras', payload.cameras);
      updateSummaryState('imus', payload.imus);
      renderSummaryList(cameraSummary, payload.cameras, 'cameras');
      renderSummaryList(imuSummary, payload.imus, 'imus');

      ensureCameraCards(payload.cameras);

      for (const camera of payload.cameras) {
        const card = cameraCards.get(camera.name);
        if (card) {
          card.ready = camera.ready;
          card.latestSequence = camera.sequence;
          card.caption.textContent = `${camera.name} ${camera.ready ? '' : '(waiting)'}`.trim();
        }
      }

      startAppButton.disabled = payload.main_app.running && !payload.fatal_error;
      stopAppButton.disabled = !payload.main_app.running || payload.logging.running;
      startLogButton.disabled = !payload.main_app.running || payload.logging.running || !payload.shared_memory_ready || Boolean(payload.fatal_error);
      stopLogButton.disabled = !payload.logging.running;
    }

    async function refreshCameraFrames() {
      try {
        await Promise.all(Array.from(cameraCards.entries(), async ([name, card]) => {
          if (!card.ready || card.inFlight || card.latestSequence === null || card.latestSequence === card.renderedSequence) {
            return;
          }

          card.inFlight = true;
          const requestedSequence = card.latestSequence;
          try {
            const response = await fetch(card.endpoint, { cache: 'no-store' });
            if (!response.ok || response.status === 204) {
              return;
            }

            const blob = await response.blob();
            if (blob.size === 0) {
              return;
            }

            const nextUrl = URL.createObjectURL(blob);
            if (card.objectUrl) {
              URL.revokeObjectURL(card.objectUrl);
            }
            card.objectUrl = nextUrl;
            card.image.src = nextUrl;
            if (card.latestSequence === requestedSequence) {
              card.renderedSequence = requestedSequence;
            }
          } finally {
            card.inFlight = false;
          }
        }));
      } catch (error) {
        void error;
      }
    }

    startAppButton.addEventListener('click', async () => {
      try {
        await postJson('/api/start_app', { config_path: configInput.value.trim() });
        await refreshState();
      } catch (error) {
        alert(error.message);
      }
    });

    stopAppButton.addEventListener('click', async () => {
      try {
        await postJson('/api/stop_app', {});
        await refreshState();
      } catch (error) {
        alert(error.message);
      }
    });

    startLogButton.addEventListener('click', async () => {
      try {
        const payload = await postJson('/api/start_logging', {
          config_path: configInput.value.trim(),
          log_dir: logDirInput.value.trim()
        });
        if (payload.log_path) {
          logPathElement.textContent = payload.log_path;
        }
        await refreshState();
      } catch (error) {
        alert(error.message);
      }
    });

    stopLogButton.addEventListener('click', async () => {
      try {
        await postJson('/api/stop_logging', {});
        await refreshState();
      } catch (error) {
        alert(error.message);
      }
    });

    async function run() {
      await refreshState();
      setInterval(() => { refreshState().catch(() => {}); }, 100);
      setInterval(() => { refreshCameraFrames().catch(() => {}); }, 33);
    }

    run().catch((error) => {
      alert(error.message);
    });
  </script>
</body>
</html>
"""


class DataCollectorServer:
    def __init__(self, host: str, port: int, config_path: str, log_dir: str):
        self._state = DataCollectorState(config_path, log_dir)
        self._state.start_background_collection()
        self._server = ThreadingHTTPServer((host, port), self._make_handler())
        self.url = f"http://{self._server.server_address[0]}:{self._server.server_address[1]}"

    def serve_forever(self) -> None:
        self._server.serve_forever()

    def close(self) -> None:
        self._state.close()
        self._server.shutdown()
        self._server.server_close()

    def _make_handler(self):
        state = self._state

        class Handler(BaseHTTPRequestHandler):
            def log_message(self, format: str, *args: Any) -> None:
                return

            def do_GET(self) -> None:
                parsed = urlparse(self.path)
                if parsed.path == "/":
                    html = WEB_HTML.replace("__DEFAULT_CONFIG__", DEFAULT_CONFIG_PATH).replace("__DEFAULT_LOG_DIR__", DEFAULT_LOG_DIR)
                    self._write(HTTPStatus.OK, "text/html; charset=utf-8", html)
                    return
                if parsed.path == "/api/state":
                    payload = state.snapshot()
                    self._write(HTTPStatus.OK, "application/json; charset=utf-8", json.dumps(payload))
                    return
                if parsed.path == "/api/camera_frame":
                    query = parse_qs(parsed.query)
                    name = query.get("name", [None])[0]
                    if not name:
                        self.send_error(HTTPStatus.BAD_REQUEST)
                        return
                    frame_bytes = state.camera_frame(name)
                    if frame_bytes is None:
                        self.send_response(HTTPStatus.NO_CONTENT)
                        self.send_header("Cache-Control", "no-store")
                        self.send_header("Content-Length", "0")
                        self.end_headers()
                        return
                    self.send_response(HTTPStatus.OK)
                    self.send_header("Content-Type", "image/jpeg")
                    self.send_header("Content-Length", str(len(frame_bytes)))
                    self.send_header("Cache-Control", "no-store")
                    self.end_headers()
                    self.wfile.write(frame_bytes)
                    return
                self.send_error(HTTPStatus.NOT_FOUND)

            def do_POST(self) -> None:
                parsed = urlparse(self.path)
                body = self._read_json()
                try:
                    if parsed.path == "/api/start_app":
                        payload = state.start_main_app(body["config_path"])
                    elif parsed.path == "/api/stop_app":
                        payload = state.stop_main_app()
                    elif parsed.path == "/api/start_logging":
                        payload = state.start_logging(body["config_path"], body["log_dir"])
                    elif parsed.path == "/api/stop_logging":
                        payload = state.stop_logging()
                    else:
                        self.send_error(HTTPStatus.NOT_FOUND)
                        return
                except Exception as error:
                    self._write(HTTPStatus.BAD_REQUEST, "application/json; charset=utf-8", json.dumps({"error": str(error)}))
                    return
                self._write(HTTPStatus.OK, "application/json; charset=utf-8", json.dumps(payload))

            def _read_json(self) -> dict[str, Any]:
                length = int(self.headers.get("Content-Length", "0"))
                if length == 0:
                    return {}
                raw = self.rfile.read(length).decode("utf-8")
                return json.loads(raw)

            def _write(self, status: HTTPStatus, content_type: str, payload: str) -> None:
                encoded = payload.encode("utf-8")
                self.send_response(status)
                self.send_header("Content-Type", content_type)
                self.send_header("Content-Length", str(len(encoded)))
                self.send_header("Cache-Control", "no-store")
                self.end_headers()
                self.wfile.write(encoded)

        return Handler


def build_arg_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Small web frontend for collecting core camera and IMU data")
    parser.add_argument("--host", default=DEFAULT_HOST)
    parser.add_argument("--port", type=int, default=DEFAULT_PORT)
    parser.add_argument("--config", default=DEFAULT_CONFIG_PATH)
    parser.add_argument("--log-dir", default=DEFAULT_LOG_DIR)
    return parser


def main() -> int:
    args = build_arg_parser().parse_args()
    server = DataCollectorServer(args.host, args.port, args.config, args.log_dir)
    print(f"Serving on {server.url}")

    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
