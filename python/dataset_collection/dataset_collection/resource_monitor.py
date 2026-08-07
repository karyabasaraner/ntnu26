"""Lightweight background CPU/RAM sampler for recording-session summaries
(Phase 2's "measure FPS, CPU, RAM, SSD speed" checkpoint). SSD write speed
isn't sampled here -- it's derived from bytes-written / duration by the
caller, since that's already known from what was just written to disk.
"""
from __future__ import annotations

import threading
import time
from dataclasses import dataclass
from typing import Any, Optional


@dataclass
class ResourceSample:
    wall_time_s: float
    cpu_percent: float
    ram_percent: float
    ram_used_mb: float


class ResourceMonitor:
    """Samples process-wide CPU/RAM usage on a background thread at a fixed
    interval, for the duration of a recording. No-ops (with a warning) if
    psutil isn't installed, so it's always safe to start/stop.
    """

    def __init__(self, interval_s: float = 1.0):
        self._interval_s = interval_s
        self._samples: list[ResourceSample] = []
        self._thread: Optional[threading.Thread] = None
        self._stop_event = threading.Event()

    @property
    def samples(self) -> list[ResourceSample]:
        return list(self._samples)

    def start(self) -> None:
        try:
            import psutil  # noqa: F401
        except ImportError:
            print("psutil not installed -- skipping CPU/RAM monitoring")
            return
        self._stop_event.clear()
        self._thread = threading.Thread(target=self._loop, daemon=True)
        self._thread.start()

    def stop(self) -> None:
        self._stop_event.set()
        if self._thread is not None:
            self._thread.join(timeout=5.0)
            self._thread = None

    def _loop(self) -> None:
        import psutil

        start = time.monotonic()
        psutil.cpu_percent(interval=None)  # prime the non-blocking measurement
        while not self._stop_event.wait(self._interval_s):
            memory = psutil.virtual_memory()
            self._samples.append(ResourceSample(
                wall_time_s=time.monotonic() - start,
                cpu_percent=psutil.cpu_percent(interval=None),
                ram_percent=memory.percent,
                ram_used_mb=memory.used / (1024 * 1024),
            ))

    def summary(self) -> dict[str, Any]:
        if not self._samples:
            return {"samples": 0}
        cpu = [s.cpu_percent for s in self._samples]
        ram_pct = [s.ram_percent for s in self._samples]
        ram_mb = [s.ram_used_mb for s in self._samples]
        return {
            "samples": len(self._samples),
            "cpu_percent_mean": sum(cpu) / len(cpu),
            "cpu_percent_max": max(cpu),
            "ram_percent_mean": sum(ram_pct) / len(ram_pct),
            "ram_used_mb_max": max(ram_mb),
        }
