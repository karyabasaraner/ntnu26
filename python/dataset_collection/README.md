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

**None of this has been run against real hardware or the real SDKs.** It was
written in a cloud sandbox with no Basler/Prophesee/BMI088 hardware and none
of the three SDKs above installed -- only `numpy`/`opencv-python`/`pyyaml`
could actually be verified as importable here. Treat every file as a
first-draft against each vendor's documented Python API, not as tested code.
Files with a `NOTE(verify-on-device)` comment flag a specific API call whose
exact name/signature could not be confirmed and should be checked first.

## Phase checkpoints

Run each from `python/dataset_collection/` with the venv active.

| Phase | Script | Output |
|---|---|---|
| 0 | `discover_cameras.py` | fills in `config/camera_info.yaml` with detected serials |
| 1 | `record_basler.py` | `<output>/images/000001.png ...`, `<output>/timestamps.csv` |
| 1 | `record_event.py` | `<output>/events.raw`, `<output>/timestamps.csv` |
| 1 | `record_imu.py` | `<output>/imu.csv` |
| 2 | `record_multi_rgb.py` | `<output>/RGB1/ ... RGB4/`, `<output>/summary.json` (FPS/dropped-frame stats) |
| 3 | `analyze_latency.py` | `<output>/latency_analysis.pdf` |
| 4 | `record_rgb_event.py` | `<output>/RGB1/ ... RGB4/`, `<output>/Event/`, `<output>/timestamps.csv` |
| 5 | `record.py` | `<output>/RGB1/ ... RGB4/`, `<output>/Event/`, `<output>/imu.csv`, `<output>/metadata.json` |

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
