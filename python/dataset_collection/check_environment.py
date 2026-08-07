#!/usr/bin/env python3
"""Environment verification for python/dataset_collection.

Run this ON THE JETSON (or wherever you intend to run the record_*.py
scripts), inside the dataset_collection venv, BEFORE trying discover_cameras.py:

    source venv/dataset_collection/bin/activate
    cd python/dataset_collection
    python check_environment.py

It checks that each SDK this toolkit needs is actually importable and
usable, reports versions where it can get them, and flags anything that
looks off against the versions recorded in your hardware/software notes
(pypylon 26.04.1, Metavision SDK 5.1.1). It does NOT check what's
physically plugged in right now -- that's discover_cameras.py's job, run
this first.

Note on Pylon specifically: there are two *separate* installs floating
around this project, easy to conflate --
  - the system Pylon SDK under /opt/pylon, needed only for the C++ `core`
    build (`pylon-config.cmake`)
  - the pip `pypylon` wheel, needed only for this Python toolkit, and
    self-contained (bundles its own runtime, does NOT require /opt/pylon)
This script only cares about the second one.
"""
from __future__ import annotations

import importlib
import subprocess
import sys
from dataclasses import dataclass
from typing import Optional


@dataclass
class CheckResult:
    name: str
    status: str  # "PASS", "WARN", "FAIL"
    detail: str


results: list[CheckResult] = []


def record(name: str, status: str, detail: str) -> None:
    results.append(CheckResult(name, status, detail))


def pip_version(package: str) -> Optional[str]:
    try:
        from importlib.metadata import version
        return version(package)
    except Exception:
        return None


def dpkg_versions(grep_pattern: str) -> list[str]:
    try:
        output = subprocess.run(
            ["dpkg-query", "-W", "-f=${Package} ${Version}\n"],
            capture_output=True, text=True, timeout=10,
        ).stdout
    except Exception:
        return []
    return [line for line in output.splitlines() if grep_pattern.lower() in line.lower()]


def check_python() -> None:
    record("python", "PASS", sys.version.split()[0])


def check_pypylon() -> None:
    version = pip_version("pypylon")
    if version is None:
        record("pypylon (pip)", "FAIL", "not installed -- `pip install pypylon` inside the venv")
        return
    status = "PASS" if version.startswith("26.") else "WARN"
    note = "" if status == "PASS" else f" (hardware notes recorded 26.04.1, you have {version} -- probably fine, just noting the difference)"
    record("pypylon (pip)", status, f"version {version}{note}")

    try:
        from pypylon import pylon
        tl_factory = pylon.TlFactory.GetInstance()
        devices = tl_factory.EnumerateDevices()
        record("pypylon runtime", "PASS", f"TlFactory usable, {len(devices)} device(s) visible right now")
    except ImportError as exc:
        record("pypylon runtime", "FAIL", f"import failed: {exc}")
    except Exception as exc:
        record("pypylon runtime", "WARN", f"imported, but TlFactory call failed: {exc}")


def check_metavision() -> None:
    module_name = None
    error = None
    for candidate in ("metavision_sdk_stream", "metavision_sdk_driver"):
        try:
            importlib.import_module(candidate)
            module_name = candidate
            break
        except ImportError as exc:
            error = exc
    if module_name is None:
        record(
            "Metavision SDK",
            "FAIL",
            f"neither metavision_sdk_stream nor metavision_sdk_driver importable ({error}) -- "
            "install the Metavision SDK system-wide (not pip); if it's installed but this still "
            "fails, the venv may need --system-site-packages (setup_venv.sh already sets this, "
            "but a pre-existing venv from before that change won't have it -- delete and re-run "
            "setup_venv.sh)",
        )
        return
    record("Metavision SDK", "PASS", f"{module_name} importable")

    deb_versions = dpkg_versions("metavision")
    if deb_versions:
        record("Metavision SDK (apt)", "PASS", "; ".join(deb_versions[:5]) + (" ..." if len(deb_versions) > 5 else ""))
    else:
        record("Metavision SDK (apt)", "WARN", "no metavision packages found via dpkg -- installed a non-apt way?")

    try:
        from metavision_hal import DeviceDiscovery
        serials = DeviceDiscovery.list()
        record("metavision_hal", "PASS", f"DeviceDiscovery usable, {len(serials)} device(s) visible right now")
    except ImportError as exc:
        record("metavision_hal", "WARN", f"metavision_hal not importable ({exc}) -- discover_cameras.py needs this")
    except Exception as exc:
        record("metavision_hal", "WARN", f"imported, but DeviceDiscovery.list() failed: {exc}")


def check_iio() -> None:
    try:
        import iio
    except ImportError as exc:
        record(
            "iio (libiio Python bindings)",
            "FAIL",
            f"not importable ({exc}) -- either `sudo apt install python3-libiio`, or build "
            "third-party/libiio with Python bindings enabled (-DPYTHON_BINDINGS=ON or similar, "
            "check that submodule's own docs/CMakeLists.txt)",
        )
        return
    version_str = getattr(iio, "version", None)
    record("iio module", "PASS", f"importable{f', version {version_str}' if version_str else ''}")

    try:
        context = iio.Context()
        device_ids = [device.id for device in context.devices]
        record("iio.Context()", "PASS", f"{len(device_ids)} device(s) visible: {device_ids}")
    except Exception as exc:
        record("iio.Context()", "WARN", f"module imports, but creating a Context failed: {exc}")


def check_toolkit_deps() -> None:
    for package in ("numpy", "cv2", "yaml", "matplotlib", "psutil"):
        try:
            module = importlib.import_module(package)
            version = getattr(module, "__version__", pip_version(package) or "unknown")
            record(package, "PASS", f"version {version}")
        except ImportError as exc:
            record(package, "FAIL", f"not importable ({exc}) -- re-run setup_venv.sh")


def print_report() -> int:
    name_width = max(len(r.name) for r in results) + 2
    print(f"\n{'CHECK'.ljust(name_width)}STATUS  DETAIL")
    print("-" * 100)
    fail_count = 0
    for r in results:
        if r.status == "FAIL":
            fail_count += 1
        print(f"{r.name.ljust(name_width)}{r.status:<8}{r.detail}")
    print("-" * 100)
    if fail_count == 0:
        print("All checks passed (or only WARN). Move on to discover_cameras.py.")
    else:
        print(f"{fail_count} FAIL(s) above -- fix those before discover_cameras.py will work for that sensor.")
    return 1 if fail_count else 0


def main() -> int:
    check_python()
    check_toolkit_deps()
    check_pypylon()
    check_metavision()
    check_iio()
    return print_report()


if __name__ == "__main__":
    raise SystemExit(main())
