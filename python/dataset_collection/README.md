# dataset_collection

Standalone Python toolkit for Phases 0-5 of the perception module's dataset
collection plan: hardware validation, individual-sensor recording, then
progressively combining Basler RGB + Prophesee event camera + BMI088 IMU into
one synchronized recording.

This is deliberately **separate from the C++ `core` stack** (`core-main` /
`core-log`). The `core` stack is the production shared-memory + MCAP
pipeline; this toolkit is the lighter-weight, quicker-to-iterate path for
learning the vendor SDKs and hitting the plan's phase checkpoints directly
against `pypylon` / the Metavision SDK / `libiio`, without a C++ rebuild each
time. Where it makes sense (Pylon timestamp handling, event clock anchoring),
it deliberately mirrors the logic already proven out in `core/modules/`.

## Setup

```bash
./python/dataset_collection/setup_venv.sh
source venv/dataset_collection/bin/activate
```

The venv is created with `--system-site-packages` because two of the three
SDKs used here are **not pip packages**:

- **pypylon** — pip-installable, listed in `pyproject.toml`.
- **Metavision SDK Python bindings** (`metavision_sdk_stream` /
  `metavision_sdk_driver`, depending on your SDK version) — installed
  system-wide by Prophesee's SDK installer. Not on PyPI.
- **`iio` Python bindings** — come from building `third-party/libiio` (already
  a submodule of this repo) with Python bindings enabled, or your system's
  `python3-libiio` package. Not on PyPI either.

If imports of `metavision_sdk_stream`/`metavision_sdk_driver` or `iio` fail,
that's the first thing to check on your Jetson.

## ⚠️ Verification status

Most of this was written in a cloud sandbox with no Basler/Prophesee/BMI088
hardware and none of the three SDKs above installed -- only
`numpy`/`opencv-python`/`pyyaml` could actually be verified as importable
there. Treat any file without a note below as a first-draft against each
vendor's documented Python API, not as tested code. Files with a
`NOTE(verify-on-device)` comment flag a specific API call whose exact
name/signature could not be confirmed and should be checked first.

**Verified on real hardware (Jetson + GenX320 + BMI088):**
- `dataset_collection/event_recorder.py`'s raw recording path
  (`start_raw_recording`/`stop_raw_recording`, used by `record_event.py`,
  Phase 1) -- the originally-guessed API (`.cd()`, `.biases()`,
  `.start_recording()`) didn't exist on the installed SDK at all; the real
  API turned out to be `device.get_i_events_stream()`, and recording also
  needed an active background poll loop (`log_raw_data()` alone writes
  nothing to disk -- confirmed via a 248-byte header-only file vs. a
  real ~2.5MB file for the same 5s window once polling was added). Both are
  fixed and confirmed capturing real events on `Event1`.
- The BMI088 IMU itself: not a bug in this toolkit, but a kernel driver crash
  on the Jetson (unrelated HTE hardware-timestamping feature killing the
  whole probe) blocked `record_imu.py`/`ImuRecorder` from ever seeing the
  accelerometer/gyroscope IIO devices. Fixed at the kernel level; both
  channels confirmed producing live physical readings (accelerometer
  correctly reads ~1g on its vertical axis at rest).

**Still unverified / known not to work yet:**
- `event_recorder.py`'s `stream_events()` (decoded CD events, used by
  Phase 4/5's `record_rgb_event.py`/`record.py`) -- same wrong-API problem
  as raw recording had, not yet worked through against real hardware.
  Raises `NotImplementedError` rather than crashing confusingly.
- Bias file loading (`--bias-file`) -- `.biases().set_from_file(...)`
  doesn't exist either; likely replacement is `device.get_i_ll_biases()`,
  unconfirmed. Raises `NotImplementedError` rather than guessing.

## Phase checkpoints

Run each from `python/dataset_collection/` with the venv active.

| Phase | Script | Output |
|---|---|---|
| 0 | `discover_cameras.py` | fills in `config/camera_info.yaml` with detected serials |
| 1 | `record_basler.py` | `<output>/images/000001.png ...`, `<output>/timestamps.csv` |
| 1 | `record_event.py` | `<output>/events.raw`, `<output>/timestamps.csv` (one event camera at a time -- pass `--camera Event1` or `--camera Event2`) |
| 1 | `record_imu.py` | `<output>/imu.csv` |
| 2 | `record_multi_rgb.py` | `<output>/RGB1/ ... RGB4/`, `<output>/summary.json` (FPS/dropped-frame stats) |
| 3 | `analyze_latency.py` | `<output>/latency_analysis.pdf` |
| 4 | `record_rgb_event.py` | `<output>/RGB1/ ... RGB4/`, `<output>/Event1/`, `<output>/Event2/`, `<output>/timestamps.csv` |
| 5 | `record.py` | `<output>/RGB1/ ... RGB4/`, `<output>/Event1/`, `<output>/Event2/`, `<output>/imu.csv`, `<output>/metadata.json` |

**Why do two event cameras show the same identifier?**

The module has 2 Prophesee GenX320 slots (`Event1`/`Event2` in
`camera_info.yaml`), but on this hardware only one has a real sensor wired
to it. `discover_cameras.py` still reports **two** event cameras, both with
the *identical* serial string (`Prophesee:hal_plugin_prophesee:genx320
9-003c`) -- confirmed via `metavision_hal.DeviceDiscovery.list()` itself
returning that string twice, so this isn't a bug in this toolkit's discovery
code.

Root cause (confirmed on-device): the GenX320s are CSI-connected, and the
Jetson boots with a device-tree overlay
(`tegra234-p3767-camera-p3768-genx320-dual.dtbo`, applied via NVIDIA's
`jetson-io.py` "Jetson Camera GENX320 Dual" hardware profile) that declares
**two** logical camera slots at the kernel level, regardless of what's
physically populated. With only one slot wired to a real sensor, the second
slot's query doesn't cleanly report "not found" -- it returns the same
serial as the real one.

**Why this is left as-is rather than "fixed":** the only way to get a
single-camera overlay applied would be hand-editing `extlinux.conf` (the
file that controls whether the Jetson boots at all) to point at a
`genx320-A.dtbo` single-camera overlay -- but `jetson-io.py`'s own list of
supported hardware profiles (checked directly: **Configure Jetson 24pin CSI
Connector → Configure for compatible hardware**) only offers "Jetson Camera
GENX320 Dual", no single-camera variant. There's no officially-supported
path to change this, and no real payoff if we did -- the toolkit already
handles it cleanly at the software level (below), and there's only one
physical sensor either way, so a "fix" would only make the discovery output
tidier, not unlock anything new.

**Current, working state:** `camera_info.yaml` leaves `Event2`'s
`serial_number` blank. Every script that drives multiple event cameras
(`MultiEventRig` in `dataset_collection/event_rig.py`) skips any slot with
no serial configured, so `Event1` runs normally and `Event2` is never
opened -- confirmed on real hardware: `record_event.py --camera Event1`
captures real events correctly (see Phase 1's verification note above).
`discover_cameras.py` prints a warning automatically whenever it sees
duplicate serials, as a safety net in case this ever changes (e.g. a second
sensor genuinely gets wired in later) -- if that warning ever refers to two
*different* serial strings instead of an identical pair, that's a sign the
underlying hardware situation has actually changed and this note should be
revisited.

All scripts take `--config config/camera_info.yaml` (serial numbers) and
`--output-dir <dir>` (defaults documented with `--help`). Re-running a script
never overwrites a previous run -- output directories get `_2`, `_3`, ...
suffixes if the target name already exists.

## Clock domain

Every recorder publishes timestamps in **one shared clock domain**: the
process's monotonic clock (`time.monotonic_ns()`, i.e. `CLOCK_MONOTONIC`).

- Basler frames: camera-tick timestamps converted via
  `GevTimestampTickFrequency` and anchored onto the monotonic clock at stream
  start (mirrors `core/modules/camera/pylon_camera.cpp`).
- Event camera CD events: device microsecond timestamps anchored onto the
  monotonic clock at the first event batch (mirrors
  `core/modules/event_camera/prophesee_event_camera.cpp`).
- IMU samples: read directly from the IIO driver's hardware `timestamp`
  channel when present -- on Linux this **is** `CLOCK_MONOTONIC` already, so
  no anchoring is needed, only a coarser host-side fallback if that channel
  isn't exposed.

None of this corrects for clock drift between a sensor's own oscillator and
the host over a long recording -- only single-point anchoring at stream
start. Good enough to answer Phase 3's "how synchronized are the cameras?"
at a first pass; revisit if `analyze_latency.py` shows drift growing over a
recording's length.

## Layout

```
dataset_collection/            # importable package: recorders + shared utilities
  clock.py                     # shared monotonic clock + device-clock anchoring
  dataset_writer.py            # output-dir / CSV / metadata.json helpers
  discovery.py                 # Phase 0: enumerate Basler + event cameras, check IMU
  basler_recorder.py           # BaslerRecorder (pypylon)
  event_recorder.py            # EventCameraRecorder (Metavision SDK)
  imu_recorder.py              # ImuRecorder (libiio)
config/camera_info.yaml        # Phase 0 deliverable: serial numbers per sensor
discover_cameras.py            # Phase 0 script
record_basler.py               # Phase 1 script (single camera)
record_event.py                # Phase 1 script
record_imu.py                  # Phase 1 script
record_multi_rgb.py            # Phase 2 script (all 4 Baslers)
analyze_latency.py             # Phase 3 script
record_rgb_event.py            # Phase 4 script
record.py                      # Phase 5 script -- the "one command" entrypoint
```
