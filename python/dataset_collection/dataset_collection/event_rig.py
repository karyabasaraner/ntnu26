"""Shared helper for driving multiple event cameras at once (this module's
hardware has 2: Event1/Event2). Mirrors rgb_rig.py's MultiBaslerRig so
Phase 4/5 scripts wire up RGB and event cameras the same way.
"""
from __future__ import annotations

from pathlib import Path
from typing import Any

from .event_record_io import EventRecordWriter
from .event_recorder import EventBatch, EventCameraRecorder

EVENT_CAMERA_NAMES = ["Event1", "Event2"]


def resolve_event_camera_serials(camera_info: dict[str, Any]) -> dict[str, str]:
    serials: dict[str, str] = {}
    for entry in camera_info.get("event_cameras", []) or []:
        name = entry.get("name")
        if name in EVENT_CAMERA_NAMES:
            serials[name] = entry.get("serial_number") or ""
    return serials


class MultiEventRig:
    """Owns one EventCameraRecorder + EventRecordWriter pair per active (has
    a configured serial_number) event camera among Event1/Event2.

    IMPORTANT, root-caused on real hardware (see README's "Why do two event
    cameras show the same identifier?"): on this module, only one GenX320
    slot has a real sensor wired to it, but the Jetson boots with a
    device-tree overlay declaring TWO logical CSI camera slots regardless,
    so discover_cameras.py reports two event cameras with an IDENTICAL
    serial string -- not a bug in discovery, and not something
    Camera.from_serial() can disambiguate. camera_info.yaml leaves the unwired
    slot's serial_number blank on purpose; this class skips any slot with no
    serial configured (see active_names below), which is the actual fix in
    use -- confirmed working: the active slot (Event1) captures real events
    correctly. Don't try the "cover a lens and see if the serial changes"
    trick here -- unlike GigE Baslers, a CSI device's reported identity
    doesn't depend on what's in front of the lens.
    """

    def __init__(self, output_dir: Path, serials: dict[str, str], bias_file: str = ""):
        self.active_names = [name for name in EVENT_CAMERA_NAMES if serials.get(name)]
        missing = [name for name in EVENT_CAMERA_NAMES if not serials.get(name)]
        if missing:
            print(f"WARNING: no serial_number for {missing} -- skipping those event cameras.")

        self.serials = serials
        self._recorders: dict[str, EventCameraRecorder] = {}
        self._writers: dict[str, EventRecordWriter] = {}
        self._bounds: dict[str, dict[str, int | None]] = {}

        for name in self.active_names:
            event_dir = output_dir / name
            event_dir.mkdir(parents=True)
            self._writers[name] = EventRecordWriter(event_dir / "events.bin")
            self._recorders[name] = EventCameraRecorder(serial_number=serials[name], bias_file=bias_file)
            self._bounds[name] = {"first_ns": None, "last_ns": None}

    def _make_on_events(self, name: str):
        writer = self._writers[name]
        bounds = self._bounds[name]

        def on_events(batch: EventBatch) -> None:
            writer.write_batch(batch)
            if batch.host_timestamp_ns.size == 0:
                return
            if bounds["first_ns"] is None:
                bounds["first_ns"] = int(batch.host_timestamp_ns.min())
            bounds["last_ns"] = int(batch.host_timestamp_ns.max())

        return on_events

    def start(self) -> None:
        for name in self.active_names:
            self._recorders[name].stream_events(self._make_on_events(name))

    def stop(self) -> None:
        for name in self.active_names:
            self._recorders[name].stop()
            self._recorders[name].close()
        for writer in self._writers.values():
            writer.close()

    def event_count(self, name: str) -> int:
        return self._writers[name].count

    def bounds(self, name: str) -> dict[str, int | None]:
        return self._bounds[name]
