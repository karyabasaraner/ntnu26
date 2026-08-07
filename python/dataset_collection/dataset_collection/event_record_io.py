"""Binary encoding for decoded events written to disk, matching the layout of
core/modules/event_camera/event_record.hpp's EventRecord (16 bytes: int64
timestamp_ns, uint16 x, uint16 y, uint8 polarity, uint8 reserved + 2 bytes of
trailing padding to reach the C++ struct's natural 16-byte size) so the
on-disk format stays consistent between the C++ core stack and this Python
toolkit, in case the two ever need to interoperate.
"""
from __future__ import annotations

from pathlib import Path

import numpy as np

from .event_types import EventBatch

EVENT_RECORD_DTYPE = np.dtype([
    ("timestamp_ns", "<i8"),
    ("x", "<u2"),
    ("y", "<u2"),
    ("polarity", "u1"),
    ("reserved", "u1"),
    ("_pad", "u1", 2),
])
assert EVENT_RECORD_DTYPE.itemsize == 16


def batch_to_records(batch: EventBatch) -> np.ndarray:
    records = np.zeros(batch.x.size, dtype=EVENT_RECORD_DTYPE)
    records["timestamp_ns"] = batch.host_timestamp_ns
    records["x"] = batch.x
    records["y"] = batch.y
    records["polarity"] = batch.polarity
    return records


class EventRecordWriter:
    """Appends EventRecord-formatted batches to a single binary file, in
    arrival order.
    """

    def __init__(self, path: str | Path):
        self._file = open(path, "wb")
        self._count = 0

    @property
    def count(self) -> int:
        return self._count

    def write_batch(self, batch: EventBatch) -> None:
        records = batch_to_records(batch)
        records.tofile(self._file)
        self._count += records.size

    def close(self) -> None:
        self._file.close()
