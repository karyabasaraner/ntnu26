"""Small shared helpers for writing checkpoint outputs: numbered output
directories, buffered per-frame/per-sample CSV timestamp logs, and
metadata.json files. Used by every record_*.py script so output layout stays
consistent across phases.
"""
from __future__ import annotations

import csv
import json
import time
from pathlib import Path
from typing import Any, Optional


def make_output_dir(base_dir: str | Path, name: str) -> Path:
    """Creates base_dir/name, or base_dir/name_2, _3, ... if that name is
    already taken -- so re-running a checkpoint script never silently
    overwrites (or worse, interleaves into) a previous recording.
    """
    base = Path(base_dir)
    base.mkdir(parents=True, exist_ok=True)
    candidate = base / name
    counter = 1
    while candidate.exists():
        counter += 1
        candidate = base / f"{name}_{counter}"
    candidate.mkdir(parents=True)
    return candidate


class CsvTimestampWriter:
    """Thin buffered CSV writer for per-frame/per-sample timestamp logs.

    Not thread-safe by itself -- callers writing from multiple recorder
    threads (e.g. record_multi_rgb.py, one thread per camera) should either
    use one writer per thread/stream, or guard `.write()` with a lock.
    """

    def __init__(self, path: str | Path, fieldnames: list[str]):
        self._path = Path(path)
        self._file = self._path.open("w", newline="", encoding="utf-8")
        self._writer = csv.DictWriter(self._file, fieldnames=fieldnames)
        self._writer.writeheader()

    def write(self, row: dict[str, Any]) -> None:
        self._writer.writerow(row)

    def flush(self) -> None:
        self._file.flush()

    def close(self) -> None:
        self._file.close()

    def __enter__(self) -> "CsvTimestampWriter":
        return self

    def __exit__(self, *_exc: object) -> None:
        self.close()


def write_metadata(path: str | Path, metadata: dict[str, Any]) -> None:
    payload = {**metadata, "written_at": time.strftime("%Y-%m-%dT%H:%M:%S")}
    Path(path).write_text(json.dumps(payload, indent=2, default=str), encoding="utf-8")


def load_yaml(path: str | Path) -> dict[str, Any]:
    import yaml

    with open(path, "r", encoding="utf-8") as handle:
        return yaml.safe_load(handle) or {}


def find_camera_serial(camera_info: dict[str, Any], name: str) -> Optional[str]:
    """Looks up a camera's serial number from a loaded camera_info.yaml by its
    RGB1/RGB2/.../Event name. Returns None (meaning "first available") if the
    entry exists but its serial hasn't been filled in yet.
    """
    for section in ("cameras", "event_cameras"):
        for entry in camera_info.get(section, []) or []:
            if entry.get("name") == name:
                serial = entry.get("serial_number") or ""
                return serial or None
    return None
