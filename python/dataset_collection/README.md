# dataset_collection

Standalone Python toolkit for Phases 0-5 of the perception module's dataset
collection plan: hardware validation, individual-sensor recording, then
progressively combining Basler RGB + Prophesee event camera + BMI088 IMU into
one synchronized recording.

**New to this repo? Start with [`HANDOFF.md`](HANDOFF.md) instead** -- a
task-oriented guide (one-time setup, exact commands for every recording/
visualization workflow, a file-by-file reference, known issues, and a
troubleshooting section) written at the end of the internship that built
this. This README covers the same ground in more reference/technical-design
form (verification status per file, the clock-domain design in depth) --
useful once you're past initial setup and want the "why" behind something.

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
- `ClockAnchor`'s drift correction (see "Clock drift correction" below) --
  confirmed on two real 2-minute, 4-camera recordings: `latency trend`
  stayed under ~2ms/min for the full recording (vs. the hundreds of ms/min
  the old fixed-rate approach would have accumulated at the drift
  magnitudes Phase 3 measured), and the warm-up anchor fix measurably
  reduced (not eliminated -- see `analyze_latency.py`'s notes) the
  constant-offset artifact found in the first pass.
- `event_recorder.py`'s `stream_events()` (decoded CD events, used by
  Phase 4/5's `record_rgb_event.py`/`record.py`) -- confirmed on real
  hardware that `metavision_core.event_io.EventsIterator` (already used to
  decode RAW files, see `visualize_events.py`) also streams LIVE directly
  when given a serial number as `input_path`: 3.37M events decoded over
  5s (~675K events/sec). Implemented on that basis rather than the
  originally-guessed `device.get_i_event_cd_decoder()` HAL path (which
  does exist, confirmed via introspection, but wasn't needed). Not yet
  run through a full `record_rgb_event.py`/`record.py` session on-device --
  see NOTE(verify-on-device) in `event_recorder.py` for what's still
  unconfirmed (mainly: whether `stop()` reliably joins the streaming
  thread promptly on a live, ongoing stream, vs. only tested against a
  short bounded capture so far).

**Still unverified / known not to work yet:**
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
  start (mirrors `core/modules/camera/pylon_camera.cpp`), **and
  drift-corrected** (see below).
- Event camera CD events: device microsecond timestamps anchored onto the
  monotonic clock at the first event batch (mirrors
  `core/modules/event_camera/prophesee_event_camera.cpp`).
- IMU samples: originally designed to read the IIO driver's hardware
  `timestamp` channel (which on Linux **is** `CLOCK_MONOTONIC` already, no
  anchoring needed) via buffered capture, mirroring the C++ core stack.
  Confirmed on real hardware this doesn't work on this deployment -- no
  `trigger/` interface exists on the device at all (likely the sensor's
  interrupt line isn't wired in the device tree), so buffered capture never
  receives data and the hardware timestamp channel is unusable outside of
  it. `imu_recorder.py` now polls each channel's `raw` attribute directly
  instead, using the host's own clock per sample (`timestamp_source` is
  always `"poll"`) -- see its module docstring for the full story and the
  real tradeoff (no hardware-precision timestamps, achieved rate bounded by
  Python loop + sysfs overhead rather than the sensor's own clocking).

### Clock drift correction

Phase 3 (`analyze_latency.py`) measured real per-camera oscillator deviation
on this hardware by comparing each Basler's own reported
`GevTimestampTickFrequency` against the others directly: on the order of
0.04% on one camera, 0.63% on another. A single fixed-rate anchor at stream
start (the original approach) lets that error grow for the entire recording
-- negligible over a 5s test clip, but this module is meant to feed long,
SLAM-relevant recordings (many minutes), where 0.63% adds up to real
cross-camera misalignment.

`ClockAnchor` (`clock.py`) now self-corrects for this instead of just
documenting it as a limitation. It still anchors the very first sample
exactly as before (so short recordings behave identically to the old code),
but every subsequent sample also feeds a running least-squares fit of the
device's *actual* ticks-to-ns rate, using the real (camera tick, host
arrival time) pairs observed during the recording -- not the camera's
nominal/reported frequency. The fit only ever refines the rate going
forward; it never revises a timestamp it already produced, so there's no
jump or discontinuity, just increasing accuracy the longer the recording
runs. This is the same idea NTP/PTP use to discipline a clock from noisy
round-trip samples, simplified to fit here (no windowing/re-anchoring
needed since a crystal oscillator's drift rate is stable over the timescales
this module records at).

Each run's `summary.json`/`metadata.json` records the refined
`clock_drift_ppm` per RGB camera (how far the fitted rate ended up from
nominal) so the correction is auditable, not just a claim -- expect numbers
in the same ballpark as Phase 3's measurements above once a recording has
enough frames to converge (a handful of seconds' worth).

**Anchor-bias fix (found via the real-hardware test above):** the first
version of this fix still pinned the anchor to a single first sample. On
real 4-camera hardware (heavy CPU load from Bayer demosaic + PNG encode)
that showed up as a constant few-ms offset in every camera's exposure ->
arrival latency (`analyze_latency.py`'s new plot showed a flat line, so no
drift growth -- but shifted below zero the whole time, which is physically
impossible: a frame can't arrive before it's exposed). Cause: frame 1
happened to have atypically low latency before the other cameras' threads
started contending for CPU, and the anchor trusted that one sample as
ground truth for the entire recording. Fixed by buffering the first 16
`(device_ticks, observed_host_ns)` pairs and anchoring on an ordinary
least-squares fit (both slope and intercept) through all of them, instead
of one point -- same principle as the drift fix, applied to the anchor
itself. A synthetic test with a deliberately atypical first sample showed
about a 3.7x reduction in the resulting bias with the 16-sample warm-up vs.
a single-sample anchor.

Event camera timestamps now go through `ClockAnchor` too, via
`stream_events()` (Phase 4/5's decoded-CD-event path) -- but only the
origin-forced incremental rate fit, not the 16-sample warm-up anchor fix
described above yet, since that's per-`ClockAnchor`-instance and
`EventCameraRecorder` constructs its own with the default (single-sample)
anchor. Also anchored once per delta_t chunk rather than per event (this
sensor produces ~675K events/sec live -- the per-sample stateful fit
can't run at that rate), then vectorized across the chunk using the
current rate estimate, which trades a small amount of intra-chunk
precision (a delta_t chunk is only ~20ms) for being fast enough to keep
up at all. Metavision's raw recording path (Phase 1) still doesn't go
through `ClockAnchor` at all -- it's an unmodified byte passthrough with
no per-event timestamps decoded on our side, so there's nothing to anchor
there by design.

## Layout

See `HANDOFF.md`'s "File-by-file reference" for a one-paragraph description
of every file below, not just the phase it belongs to.

```
dataset_collection/            # importable package: recorders + shared utilities
  __init__.py
  clock.py                     # shared monotonic clock + device-clock anchoring (ClockAnchor)
  dataset_writer.py            # output-dir / CSV / metadata.json helpers
  discovery.py                 # Phase 0: enumerate Basler + event cameras, check IMU
  basler_recorder.py           # BaslerRecorder (pypylon)
  rgb_rig.py                   # MultiBaslerRig -- drives all 4 RGB1-4 at once
  event_recorder.py            # EventCameraRecorder (Metavision SDK) -- raw + decoded paths
  event_rig.py                 # MultiEventRig -- drives Event1/Event2 at once
  event_types.py                # EventBatch dataclass (SDK-independent)
  event_record_io.py           # on-disk binary format for decoded events (events.bin)
  event_denoise.py             # spatiotemporal noise filter shared by both visualize_*.py scripts
  imu_recorder.py               # ImuRecorder (libiio, polling-based -- see its own docstring)
  resource_monitor.py          # background CPU/RAM sampler for summary.json
config/camera_info.yaml        # Phase 0 deliverable: serial numbers per sensor (NOT committed)
setup_venv.sh                  # one-time venv setup
pyproject.toml                 # pip package metadata for `pip install -e .`
check_environment.py           # verifies every SDK import works, run before Phase 0
discover_cameras.py            # Phase 0 script
record_basler.py               # Phase 1 script (single RGB camera)
record_event.py                # Phase 1 script (single event camera, raw)
record_imu.py                  # Phase 1 script
record_multi_rgb.py            # Phase 2 script (all 4 Baslers)
analyze_latency.py             # Phase 3 script
record_rgb_event.py            # Phase 4 script
record.py                      # Phase 5 script -- the "one command" entrypoint
visualize_events.py            # decodes a single raw events.raw file (needs Metavision SDK)
visualize_dataset.py           # combined RGB video + event video + IMU graphs for a full session
```
