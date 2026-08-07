"""Phase 0: hardware discovery. Enumerates connected Basler cameras and
Prophesee event cameras, and checks whether the configured IMU IIO devices
are present -- without opening/streaming from any of them.

NOTE(verify-on-device): `metavision_hal.DeviceDiscovery.list()` is the
standard Prophesee sample pattern for listing connected serials without
opening a camera, but the exact module path can vary by SDK version (see
event_recorder.py's import fallback for the same caveat). Never run against
real hardware.
"""
from __future__ import annotations

from dataclasses import dataclass, field
from typing import Optional


@dataclass
class DiscoveredBasler:
    serial_number: str
    ip_address: str
    model_name: str
    friendly_name: str


@dataclass
class DiscoveredEventCamera:
    serial_number: str
    model_name: str = ""


@dataclass
class ImuCheckResult:
    device_id: str
    found: bool
    channels: list[str] = field(default_factory=list)
    error: Optional[str] = None


def discover_baslers() -> list[DiscoveredBasler]:
    """Enumerates all Basler devices currently reachable via GigE discovery
    broadcast. Does not open any camera.
    """
    from pypylon import pylon

    devices = pylon.TlFactory.GetInstance().EnumerateDevices()
    return [
        DiscoveredBasler(
            serial_number=device.GetSerialNumber(),
            ip_address=device.GetIpAddress() if device.IsIpAddressAvailable() else "",
            model_name=device.GetModelName(),
            friendly_name=device.GetFriendlyName(),
        )
        for device in devices
    ]


def discover_event_cameras() -> list[DiscoveredEventCamera]:
    """Enumerates connected Prophesee event cameras without opening one."""
    try:
        from metavision_hal import DeviceDiscovery
    except ImportError as exc:
        raise ImportError(
            "metavision_hal is required for event camera discovery (part of the "
            "Metavision SDK's system-wide install, not pip)."
        ) from exc

    serials = DeviceDiscovery.list()
    return [DiscoveredEventCamera(serial_number=serial) for serial in serials]


def check_imu_device(device_id: str, channel_names: list[str]) -> ImuCheckResult:
    """Checks whether an IIO device (e.g. "iio:device0") exists and exposes
    the expected channels, without starting a capture.
    """
    try:
        import iio
    except ImportError as exc:
        return ImuCheckResult(device_id=device_id, found=False, error=f"iio module not importable: {exc}")

    try:
        context = iio.Context()
        device = context.find_device(device_id)
    except Exception as exc:  # pragma: no cover - depends on real hardware
        return ImuCheckResult(device_id=device_id, found=False, error=str(exc))

    if device is None:
        return ImuCheckResult(device_id=device_id, found=False, error="device not found")

    available = [channel.id for channel in device.channels]
    missing = [name for name in channel_names if name not in available]
    if missing:
        return ImuCheckResult(
            device_id=device_id,
            found=True,
            channels=available,
            error=f"missing expected channels: {missing}",
        )
    return ImuCheckResult(device_id=device_id, found=True, channels=available)
