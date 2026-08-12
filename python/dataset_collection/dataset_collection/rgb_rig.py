"""Shared helper for driving all 4 Basler cameras at once. Used by Phase 2's
record_multi_rgb.py and reused as-is by Phase 4/5's combined recordings, so
the per-camera wiring (image + timestamp writing, dropped-frame tracking)
isn't duplicated across scripts.
"""
from __future__ import annotations

import time
from pathlib import Path
from typing import Any

import cv2

from .basler_recorder import BaslerFrame, BaslerRecorder
from .clock import now_ns, now_wall_iso
from .dataset_writer import CsvTimestampWriter

CAMERA_NAMES = ["RGB1", "RGB2", "RGB3", "RGB4"]

TIMESTAMPS_FIELDNAMES = [
    "index", "filename", "camera_timestamp_ticks",
    "exposure_host_ns", "arrival_host_ns", "disk_write_complete_ns", "wall_time_iso",
]


def resolve_camera_serials(camera_info: dict[str, Any]) -> dict[str, str]:
    serials: dict[str, str] = {}
    for entry in camera_info.get("cameras", []) or []:
        name = entry.get("name")
        if name in CAMERA_NAMES:
            serials[name] = entry.get("serial_number") or ""
    return serials


class MultiBaslerRig:
    """Owns one BaslerRecorder + image/CSV writer pair per active (has a
    configured serial_number) camera among RGB1-4.
    """

    def __init__(
        self,
        output_dir: Path,
        serials: dict[str, str],
        fps: float = 30.0,
        exposure_us: float = 2000.0,
        gain: float = 1.0,
    ):
        self.active_names = [name for name in CAMERA_NAMES if serials.get(name)]
        missing = [name for name in CAMERA_NAMES if not serials.get(name)]
        if missing:
            print(f"WARNING: no serial_number for {missing} -- skipping those cameras.")

        self.serials = serials
        self._recorders: dict[str, BaslerRecorder] = {}
        self._csv_writers: dict[str, CsvTimestampWriter] = {}
        self._images_dirs: dict[str, Path] = {}

        for name in self.active_names:
            camera_dir = output_dir / name
            images_dir = camera_dir / "images"
            images_dir.mkdir(parents=True)
            self._images_dirs[name] = images_dir
            self._csv_writers[name] = CsvTimestampWriter(camera_dir / "timestamps.csv", TIMESTAMPS_FIELDNAMES)
            self._recorders[name] = BaslerRecorder(
                serial_number=serials[name], exposure_time_us=exposure_us, gain=gain, fps=fps
            )

    def _make_on_frame(self, name: str):
        images_dir = self._images_dirs[name]
        csv_writer = self._csv_writers[name]

        def on_frame(frame: BaslerFrame) -> None:
            filename = f"{frame.index:06d}.png"
            cv2.imwrite(str(images_dir / filename), cv2.cvtColor(frame.image, cv2.COLOR_RGB2BGR))
            disk_write_complete_ns = now_ns()
            csv_writer.write({
                "index": frame.index,
                "filename": filename,
                "camera_timestamp_ticks": frame.camera_timestamp_ticks,
                "exposure_host_ns": frame.exposure_host_ns,
                "arrival_host_ns": frame.arrival_host_ns,
                "disk_write_complete_ns": disk_write_complete_ns,
                "wall_time_iso": now_wall_iso(),
            })

        return on_frame

    def start(self) -> None:
        # Cameras are started sequentially, so an earlier camera in
        # active_names has already been actively grabbing for however long
        # the later cameras took to connect/configure -- confirmed on real
        # hardware: a single shared start_time in the caller inflated the
        # first-started camera's fps_achieved to 39fps against a configured
        # 30fps cap. Tracking each camera's own start time (right after ITS
        # start() call, not after the whole loop) is what makes per-camera
        # duration/fps accurate.
        self._start_times: dict[str, float] = {}
        for name in self.active_names:
            self._recorders[name].start(self._make_on_frame(name))
            self._start_times[name] = time.monotonic()

    def start_time(self, name: str) -> float:
        return self._start_times[name]

    def stop(self) -> None:
        for name in self.active_names:
            self._recorders[name].stop()
            self._recorders[name].close()
        for writer in self._csv_writers.values():
            writer.close()

    def dir_size_bytes(self, name: str) -> int:
        return sum(f.stat().st_size for f in self._images_dirs[name].rglob("*") if f.is_file())

    def frame_count(self, name: str) -> int:
        return self._recorders[name].frame_count

    def dropped_count(self, name: str) -> int:
        return self._recorders[name].dropped_count
