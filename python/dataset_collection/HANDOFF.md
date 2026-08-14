# Handoff Guide: Perception Module Dataset Collection

Written at the end of a summer internship for whoever picks this up next.
This is the practical, task-oriented companion to `README.md` (which covers
verification status and the clock-domain design in more technical depth) --
start here, use `README.md` when you want the "why" behind something in more
detail.

**Hardware this was built and tested against:** Jetson Orin Nano Super, 4x
Basler dart GigE RGB cameras, 2x Prophesee GenX320 event cameras (only 1
physically wired in on this rig), 1x BMI088 IMU (I2C).

**Everything in this guide was actually run and confirmed working on that
real hardware** during this internship, not just written against
documentation -- where something is a known limitation instead, it's called
out explicitly rather than glossed over.

---

## Table of contents

1. [One-time setup on a fresh Jetson](#1-one-time-setup-on-a-fresh-jetson)
2. [File-by-file reference](#2-file-by-file-reference)
3. [Recording -- one sensor at a time](#3-recording----one-sensor-at-a-time)
4. [Recording -- multiple sensors together](#4-recording----multiple-sensors-together)
5. [The full recommended session workflow](#5-the-full-recommended-session-workflow)
6. [Visualization](#6-visualization)
7. [Event camera tuning cheat sheet](#7-event-camera-tuning-cheat-sheet)
8. [Known issues and limitations](#8-known-issues-and-limitations)
9. [Troubleshooting quick reference](#9-troubleshooting-quick-reference)
10. [Git workflow used during development](#10-git-workflow-used-during-development)

---

## 1. One-time setup on a fresh Jetson

### 1.1 Where this fits in the repo

This toolkit lives at `python/dataset_collection/` and is **deliberately
separate** from the C++ `core` stack (`core-main`/`core-log`) elsewhere in
this repo. The C++ stack is the production shared-memory + MCAP pipeline;
this Python toolkit is the lighter-weight path used to validate each sensor
against its vendor SDK directly, without a C++ rebuild every time. Nothing
here writes into the C++ stack's build or touches its config files.

### 1.2 Python environment

```bash
cd <repo root>
./python/dataset_collection/setup_venv.sh
source venv/dataset_collection/bin/activate
```

`setup_venv.sh` creates `venv/dataset_collection/` with `--system-site-packages`
(see `python/dataset_collection/setup_venv.sh`) and `pip install -e
python/dataset_collection` (its `pyproject.toml` lists `numpy`,
`opencv-python`, `pyyaml`, `matplotlib`, `pypylon`, `psutil` as pip deps).
`--system-site-packages` matters because **two of the three vendor SDKs are
not pip packages** and must already be installed system-wide before the venv
can see them:

- **Metavision SDK** (`metavision_sdk_stream` / `metavision_sdk_driver`,
  `metavision_hal`, `metavision_core`) -- installed by Prophesee's SDK
  installer, system Python site-packages.
- **`iio` (libiio Python bindings)** -- from `apt install python3-libiio` or
  building `third-party/libiio` with Python bindings enabled.
- **`h5py`** -- needed transitively by `metavision_core.event_io` (an
  unconditional import inside that package, even though HDF5 itself isn't
  used) -- `pip install h5py` if missing.
- **Pillow** -- needed for `visualize_events.py --gif`, usually already
  present as a matplotlib dependency.

Run `python check_environment.py` right after activating the venv -- it
checks every one of the above is actually importable and usable (and prints
versions), without touching any physically-connected hardware. Fix anything
it reports as `FAIL` before moving on; `WARN` is usually fine to ignore.

### 1.3 IMU permissions (udev rule -- needed once per Jetson, not per clone)

The BMI088 IMU is exposed via IIO character devices (`/dev/iio:device0`,
`/dev/iio:device1`) and sysfs control files that are **root-only by
default**. Without this rule, `imu_recorder.py` fails with `PermissionError`.
Confirmed necessary and sufficient on real hardware:

```bash
echo 'SUBSYSTEM=="iio", GROUP="i2c", MODE="0660", RUN+="/bin/chgrp -R i2c /sys%p", RUN+="/bin/chmod -R g+rw /sys%p"' | sudo tee /etc/udev/rules.d/99-iio-permissions.rules
sudo udevadm control --reload-rules
sudo udevadm trigger --subsystem-match=iio --action=add
```

This grants the `i2c` group read/write on both the IIO device nodes AND
their sysfs control files (`buffer/enable`, `scan_elements/*_en`,
`sampling_frequency`, etc. -- a device-level udev rule alone only fixes the
`/dev` node, NOT the sysfs tree underneath it, which is what actually needs
write access; see `RUN+=` above). Your user account needs to be in the `i2c`
group for this to matter (`groups` to check; `sudo usermod -aG i2c $USER`
and re-login if not).

**Important:** `--action=add` matters -- `udevadm trigger`'s default action
(`change`) was confirmed on real hardware to NOT re-apply permissions to a
device node that already exists from boot. If you ever re-apply this rule
and `ls -la /dev/iio:device0` still shows `root root`, re-run with
`--action=add` explicitly, not just `udevadm trigger --subsystem-match=iio`.

This rule is **not part of the git repo** -- it's a one-time system
configuration step on the Jetson itself. If you ever image a fresh Jetson or
reflash this one, redo this step.

### 1.4 BMI088 kernel driver fix (already applied to this Jetson -- read this if you ever reflash)

The stock BMI088 kernel driver on this Jetson's BSP crashed on probe (an
unrelated HTE hardware-timestamping feature killing the whole driver load) --
**this was fixed at the kernel level on this specific Jetson already; you
don't need to do anything for it to keep working as-is.** It is documented
here only so that if this Jetson is ever reflashed or the kernel is rebuilt
from a clean BSP checkout, you know this fix needs to be reapplied -- **it
is not tracked in this git repo**, it lives only in the Jetson's own
`/lib/modules/` and the BSP source tree on its filesystem.

If you ever need to redo it: the pristine driver source is at
`~/Downloads/jetson_bsp/Linux_for_Tegra/source/nvidia-oot/drivers/bmi088/bmi088_core.c`
on this Jetson (edit **this** copy, not the build-output copy under
`~/genx320_build/` -- `nvbuild.sh` syncs FROM the pristine source INTO the
build output, not the other way around, so edits to the build-output copy
get silently overwritten on the next build). Two small changes made:
`hte_ts_get`'s and `devm_hte_request_ts_ns`'s error-handling blocks changed
from fatal (`dev_err` + early `return ret;`) to non-fatal (`dev_warn`, no
early return), plus a `ret = 0;` added after the HTE setup loop so a stale
non-zero `ret` from those now-non-fatal warnings doesn't still fail the
whole probe. Rebuild with
`./nvbuild.sh -m -o ~/genx320_build` from
`~/Downloads/jetson_bsp/Linux_for_Tegra/source`, then copy the built
`bmi088.ko` over `/lib/modules/<kernel-version>/updates/drivers/bmi088/bmi088.ko`
and run `sudo depmod -a`.

Separately: after this driver loaded correctly, the accelerometer still read
all zeros until the IIO buffer's per-channel `_en` flags AND `buffer/enable`
were both set to `1` -- this is what actually moves the sensor's internal
`ACC_PWR_CONF`/`ACC_PWR_CTRL` registers out of suspend into active
conversion. `imu_recorder.py`'s `open()` already does this for you every
time it runs (see file-by-file reference below) -- this note is just
explaining *why* that code does what it does, in case you're debugging a
"readings are frozen/all zero" symptom from scratch on different hardware.

### 1.5 Hardware inventory (`config/camera_info.yaml`)

This file records which physical sensor (serial number) is which logical
role (`RGB1`-`RGB4`, `Event1`/`Event2`, `IMU`). **It is not committed to
git** (machine-specific) -- start from the template already at
`config/camera_info.yaml` and fill it in via:

```bash
python discover_cameras.py --config config/camera_info.yaml
```

This enumerates connected Baslers and event cameras (does NOT open/stream
from anything) and writes the raw results to
`config/camera_info.discovered.yaml` for you to cross-reference by hand into
`camera_info.yaml`'s `RGB1`-`RGB4`/`Event1`/`Event2` slots -- deliberately
does **not** auto-fill by discovery order, since GigE discovery order isn't
guaranteed to match physical camera position (see the script's own
docstring). Once you've confirmed the ordering (by IP address, or covering
one lens at a time and re-running), `--auto-assign` will write discovered
serials into the empty slots for you.

On this rig, RGB1-4 are assigned by ascending serial number as a practical
convention (not a hardware requirement) -- if you physically move cameras
around, re-run discovery and re-check which serial ended up on which cable.

The IMU's `accel_device`/`gyro_device`/channel names/scale factors in the
template are already filled in correctly for this BMI088 -- don't need to
be rediscovered, just left as-is.

---

## 2. File-by-file reference

### Top-level scripts (run these directly)

| File | What it does |
|---|---|
| `check_environment.py` | Verifies every SDK import works (§1.2). Run first on a new setup. |
| `discover_cameras.py` | Phase 0: enumerates connected Baslers/event cameras, checks IMU IIO devices exist. Writes `camera_info.discovered.yaml`. |
| `record_basler.py` | Phase 1: records ONE Basler camera by name/serial. `--camera RGB1` or `--serial <n>`. |
| `record_event.py` | Phase 1: records ONE event camera's **raw** byte stream (`events.raw`) -- no decoding, most robust capture path. `--camera Event1`. |
| `record_imu.py` | Phase 1: records accel + gyro to one `imu.csv`. |
| `record_multi_rgb.py` | Phase 2: all 4 Baslers at once, `RGB1/`-`RGB4/` + `summary.json` (fps/drops/CPU/RAM/clock drift per camera). |
| `analyze_latency.py` | Phase 3: reads one or more `timestamps.csv` files, produces `latency_analysis.pdf` (pipeline latency histograms, frame-interval jitter, cross-camera sync, clock-drift-over-time trend). |
| `record_rgb_event.py` | Phase 4: RGB1-4 + event camera(s), decoded events (`events.bin`), no IMU. |
| `record.py` | Phase 5: **the main entrypoint** -- RGB1-4 + event camera(s) + IMU, all in one command, one shared clock domain. |
| `visualize_events.py` | Decodes a **raw** `events.raw` file (from `record_event.py`) into a heatmap PNG + optional animated GIF. Requires the Metavision SDK. |
| `visualize_dataset.py` | Combined visualization for a full `record.py`/`record_rgb_event.py`/`record_multi_rgb.py` session: RGB videos, event video, IMU graphs, all at once. Does NOT need the Metavision SDK (reads already-decoded `events.bin`, not raw). |

### `dataset_collection/` package (imported by the scripts above, not run directly)

| File | What it does |
|---|---|
| `__init__.py` | Just a docstring pointer back to this README. |
| `clock.py` | The shared clock domain. `now_ns()`/`now_wall_iso()`, and `ClockAnchor` -- anchors a device's own tick-based clock onto the host's monotonic clock, self-correcting for oscillator drift and using a 16-sample warm-up window to avoid anchor bias from one noisy first sample. See README.md's "Clock domain" section for the full design story. |
| `dataset_writer.py` | Small shared helpers: `make_output_dir` (numbered run dirs, never overwrites), `CsvTimestampWriter`, `write_metadata` (writes `*.json` with a `written_at` stamp), `load_yaml`, `find_camera_serial`. |
| `discovery.py` | The actual enumeration logic behind `discover_cameras.py` -- `discover_baslers()`, `discover_event_cameras()`, `check_imu_device()`. |
| `basler_recorder.py` | `BaslerRecorder` -- one Basler camera via pypylon. Auto-detects the camera's actual supported Bayer format (confirmed on real hardware that nominally-identical camera units can support different Bayer variants) instead of hardcoding one. Converts camera ticks to host time via `ClockAnchor`. |
| `rgb_rig.py` | `MultiBaslerRig` -- owns one `BaslerRecorder` + image/CSV writer per active RGB1-4 camera. Used by `record_multi_rgb.py`, `record_rgb_event.py`, `record.py`. If a later camera fails to start, already-started ones get stopped cleanly instead of being left running. |
| `event_recorder.py` | `EventCameraRecorder` -- one Prophesee event camera via the Metavision SDK. `start_raw_recording()`/`stop_raw_recording()` (Phase 1 raw passthrough) and `stream_events()` (decoded CD events, Phase 4/5) are genuinely different code paths -- see the module's own docstring for exactly which API each one actually uses and why (both were originally guessed wrong against the SDK and had to be corrected against the real installed bindings). |
| `event_rig.py` | `MultiEventRig` -- same pattern as `rgb_rig.py` but for event cameras. Explains (in its own docstring) why `Event1`/`Event2` report the same serial number on this hardware and why that's expected, not a bug. |
| `event_types.py` | Just the `EventBatch` dataclass (`x`, `y`, `polarity`, `host_timestamp_ns` numpy arrays) -- split out so code that only needs the *shape* of decoded events doesn't need the Metavision SDK importable. |
| `event_record_io.py` | On-disk binary format for decoded events (`events.bin`): 16-byte packed records (`int64` host timestamp, `uint16` x, `uint16` y, `uint8` polarity + padding), matching the C++ core stack's `EventRecord` layout. `EventRecordWriter` is what `stream_events()`'s output actually gets written through. |
| `event_denoise.py` | `denoise_keep_mask()` -- the spatiotemporal activity filter shared by `visualize_events.py` and `visualize_dataset.py` (kept dependency-free of the Metavision SDK on purpose, see §7 for how it actually works and why the defaults are what they are). |
| `imu_recorder.py` | `ImuRecorder` -- one IIO IMU device (accel or gyro; `record.py`/`record_imu.py` each instantiate two). **Read this file's module docstring if you touch IMU code at all** -- it documents in detail why this polls `raw` directly instead of using the IIO buffered-capture API the way the C++ core stack does (this hardware doesn't support triggered capture at all), and why both channel-enable AND buffer-enable still have to be set even though the buffer's *data* path is never used (that's what wakes the sensor's ADC into continuous conversion -- without it, `raw` reads are frozen/stale). |
| `resource_monitor.py` | `ResourceMonitor` -- background CPU/RAM sampler used by `record_multi_rgb.py`'s `summary.json`. No-ops safely if `psutil` isn't installed. |

---

## 3. Recording -- one sensor at a time

All commands below assume you're in `python/dataset_collection/` with the
venv active (`source ../../venv/dataset_collection/bin/activate` or however
your shell is positioned relative to the repo root).

**Record one RGB camera:**
```bash
python record_basler.py --camera RGB1 --duration 10
```
Output: `output/record_basler/run_N/images/000001.png ...`,
`output/record_basler/run_N/timestamps.csv`.
Use `--serial <n>` instead of `--camera` to bypass the `camera_info.yaml`
lookup entirely. `--fps`/`--exposure-us`/`--gain` are tunable; `--num-frames`
stops after N frames instead of by wall-clock duration.

**Record one event camera (raw, most robust):**
```bash
python record_event.py --camera Event1 --duration 10
```
Output: `output/record_event/run_N/events.raw` (the sensor's native RAW
byte stream, undecoded), `timestamps.csv` (a session log, NOT per-event
data -- the RAW file itself already has full per-event timestamps in
Prophesee's own format). This is the path to use for a quick single-camera
event test, or if you specifically want the undecoded raw stream.

**Record IMU alone:**
```bash
python record_imu.py --duration 10
```
Output: `output/record_imu/run_N/imu.csv` (columns: `sensor` [`accel`/
`gyro`], `index`, `x`, `y`, `z`, `host_timestamp_ns`, `timestamp_source`
[always `"poll"` on this hardware -- see §8], `wall_time_iso`).
**Move the sensor during the recording** -- a stationary IMU only ever
reads ~1g of gravity on one axis with near-zero gyro, which doesn't tell
you much beyond "it's not completely dead." Tilt/rotate it to see the
accelerometer's gravity component shift between axes, and give it a couple
of quick shakes for a clear gyroscope spike.

---

## 4. Recording -- multiple sensors together

**All 4 RGB cameras:**
```bash
python record_multi_rgb.py --duration 10
```
Output: `output/record_multi_rgb/run_N/RGB1/ ... RGB4/`, each with its own
`images/` + `timestamps.csv`, plus `summary.json` (per-camera frame
count/dropped count/achieved fps/`clock_drift_ppm`, plus CPU/RAM/SSD
throughput over the whole recording).

**RGB + event camera(s), no IMU:**
```bash
python record_rgb_event.py --duration 10
```
Output: `output/record_rgb_event/dataset_N/RGB1/ ... RGB4/`,
`Event1/events.bin` (decoded, NOT raw -- see §2's `event_record_io.py`
entry), `Event2/` only if a serial is configured for it, plus a top-level
`timestamps.csv` (a per-stream manifest: name/count/first-last timestamp,
NOT a merged per-frame table -- each stream's own detailed data lives
alongside it).

**Everything together -- RGB + event camera(s) + IMU (the main one):**
```bash
python record.py --duration 10
```
Output: `output/record/dataset_N/RGB1/ ... RGB4/`, `Event1/`, `Event2/`
(if configured), `imu.csv`, top-level `timestamps.csv` manifest, and
`metadata.json` (session-level summary: config used, duration,
per-stream counts including each RGB camera's `clock_drift_ppm`).
`--skip-imu` records RGB + event only through this same script if you want
`record.py`'s exact RGB/event behavior without the IMU. `--bias-file` for
event camera bias tuning is **not implemented yet** (raises
`NotImplementedError` on purpose rather than guessing -- see §8).

**During any multi-sensor recording that includes both an event camera and
the IMU**: wave a hand (or anything else) in front of the event camera's
lens partway through -- event cameras only produce output where brightness
is *changing*, a static scene gives you almost nothing to visualize
afterward. Separately (doesn't need to be simultaneous), tilt/shake the IMU
module for a few seconds so its recording has real dynamic motion in it too,
not just a resting reading.

If a component fails partway through startup (e.g. an IMU permission
issue), `record.py`/`record_rgb_event.py` will print `ERROR during startup
-- stopping whatever already started` and cleanly stop anything that DID
start, rather than crashing the whole process with everything else left
running unstopped -- confirmed on real hardware this used to crash the
Python interpreter itself ("Aborted (core dumped)") when a partial failure
left RGB/event streams running with nothing to close them.

---

## 5. The full recommended session workflow

1. `source venv/dataset_collection/bin/activate`, `cd python/dataset_collection`
2. `python check_environment.py` -- confirm nothing's broken before you start (only needed after a fresh setup or if something changed system-side).
3. `python discover_cameras.py` -- confirm all expected serials are still showing up (worth doing even on a rig you haven't touched, in case a cable came loose).
4. `python record.py --duration <N>` -- the actual recording. Wave a hand for the event camera, tilt/shake the IMU (see §4).
5. `python visualize_dataset.py --input-dir output/record/dataset_N` -- see §6 for exactly what this produces and how to view it.
6. Optionally, `python analyze_latency.py output/record/dataset_N/RGB1/timestamps.csv output/record/dataset_N/RGB2/timestamps.csv ...` (all 4 RGB cameras' `timestamps.csv` paths) if you specifically want the sync/latency PDF -- not needed for routine data collection, more of a "is the clock-drift correction still behaving" spot check.

---

## 6. Visualization

### 6.1 Combined session visualization (`visualize_dataset.py`) -- use this one

```bash
python visualize_dataset.py --input-dir output/record/dataset_7
```

Auto-detects whichever streams are present (works on `record.py`,
`record_rgb_event.py`, or `record_multi_rgb.py` output -- doesn't care which
one produced the directory) and writes into
`output/record/dataset_7/visualization/`:

- `RGB1.mp4` ... `RGB4.mp4` -- one playable video per RGB camera, built
  directly from its saved PNG frames. FPS defaults to that camera's own
  **actual achieved** fps (computed from its `timestamps.csv`), not the
  configured target -- pass `--fps <n>` to force one fixed rate across all
  cameras instead (e.g. for side-by-side comparison).
- `Event1.mp4` (`Event2.mp4` if present) -- a playable video decoded from
  `events.bin`, red = "got brighter", blue = "got darker", one frame per
  `--event-delta-t-ms` window. Denoised by default -- see §7.
- `imu.png` -- two stacked plots, accelerometer x/y/z and gyroscope x/y/z
  over elapsed time.

**Videos are transcoded to H.264 automatically** (via `ffmpeg -c:v libx264`,
confirmed available on this Jetson) so they play directly in VS Code's
built-in preview and in browsers -- OpenCV's own default codec (`mp4v`)
does NOT play in either (confirmed: "video format is not supported" in VS
Code), and OpenCV's own attempt at writing H.264 directly
(`avc1`/`h264_v4l2m2m`, this Jetson's hardware encoder) fails outright
("Could not find a valid device") so don't bother trying that route again.
If `ffmpeg` isn't on PATH for some reason, this falls back to the raw
`mp4v` file with a warning printed -- still viewable in VLC or a similar
player, just not VS Code's inline preview. `--no-h264` skips the transcode
step entirely if you want faster runs and don't care about broad
playability.

**To actually view the output:** click the `.mp4` file in VS Code's
Explorer sidebar -- recent VS Code versions preview video inline, including
over Remote-SSH. If it says "not supported," you're probably looking at a
stale cached preview tab from before this fix -- close the tab and reopen
it, or reload the VS Code window (`Ctrl+Shift+P` → "Developer: Reload
Window"). `imu.png` previews the same way any image does.

### 6.2 Single raw event recording (`visualize_events.py`)

Only needed if you used `record_event.py` directly (the raw, undecoded
path) rather than going through `record.py`/`record_rgb_event.py`. Requires
the Metavision SDK (unlike `visualize_dataset.py`).

```bash
python visualize_events.py --input output/record_event/run_10/events.raw --gif
```

Produces a static PNG (polarity-colored heatmap + event-rate-over-time
plot) and, with `--gif`, an animated GIF that plays back like an actual
video of what the sensor saw (one frame per `--delta-t-ms` window, not
accumulated).

### 6.3 Sync/latency analysis (`analyze_latency.py`)

```bash
python analyze_latency.py \
    output/record/dataset_7/RGB1/timestamps.csv \
    output/record/dataset_7/RGB2/timestamps.csv \
    output/record/dataset_7/RGB3/timestamps.csv \
    output/record/dataset_7/RGB4/timestamps.csv
```

Produces `latency_analysis.pdf` next to the inputs: per-camera pipeline
latency histograms (exposure→arrival→disk-write), inter-frame interval
jitter, a **clock-drift check plot** (exposure→arrival latency over the
whole recording -- should stay flat; a rising/falling trend would mean the
drift correction isn't working), and cross-camera synchronization error
(nearest-match on exposure timestamps, since cameras run free-running, not
hardware-triggered). Single-camera input works too (just skips the
cross-camera section). This is diagnostic/spot-check tooling, not something
you need to run on every recording.

---

## 7. Event camera tuning cheat sheet

Event cameras don't have exposure/gain (no frame to expose) -- what's
"tunable" on the sensor itself is a different set of parameters called
**biases** (contrast threshold, bandwidth, refractory period), which this
toolkit does **not** currently support setting (`--bias-file` raises
`NotImplementedError` -- see §8). What you actually have control over today
is the **visualization-side** filtering, in `visualize_events.py` and
`visualize_dataset.py`:

| Flag | What it does | Confirmed-good default |
|---|---|---|
| `--delta-t-ms` / `--event-delta-t-ms` | How many ms of events go into one accumulated frame. Smaller = crisper motion snapshots, sparser-looking; larger = denser/smoother, more motion blur. | `45` |
| `--denoise-us` | Drop an event unless a nearby pixel fired within this many microseconds before it (a "does this have recent spatial/temporal support" test). `0` disables denoising entirely. | `10000` (10ms) |
| `--denoise-radius` | How large a neighborhood (in pixels) counts as "nearby" for the above. | `10` (a 21x21 neighborhood) |
| `--denoise-min-neighbors` | Require this many DISTINCT nearby pixels to have fired recently, not just one. | `1` |
| `--event-dot-radius` (`visualize_dataset.py` only) | Dilates each event into a small blob before writing to video -- needed because single-pixel colored dots get washed out by video codec compression otherwise (confirmed: max channel value only reached ~120/255 without this). | `1` (3x3 blob) |

**Why these specific numbers** (worth knowing if you ever need to re-tune
for a different sensor/scene): the first, "reasonable-on-paper" attempt
(20ms delta-t, 1px denoise radius) barely reduced visible background noise
even after raising `--denoise-min-neighbors` to 2 -- this sensor's noise
turned out to have enough of its own local spatial correlation that a tight
single-neighbor-or-two test couldn't reliably tell it apart from real
motion. What actually worked, confirmed via side-by-side comparison on real
recordings: a much **wider** neighborhood (`radius=10`) with a **lenient**
neighbor-count requirement (`min_neighbors=1`) -- a real moving edge lights
up a wide surrounding area over consecutive frames, while genuinely
isolated single-pixel noise (far from anything else, in a large radius)
still gets correctly dropped. If you're tuning for different lighting/scene
content, start from these defaults, not the smaller radius/tighter window
combination -- that one is confirmed *worse* on this hardware, not just
"an alternative."

To see the raw, undenoised decode for comparison: pass `--denoise-us 0`.

---

## 8. Known issues and limitations

Real, confirmed things to keep in mind -- none of these block routine data
collection, but they matter if you're relying on this data for anything
precision-sensitive (e.g. SLAM).

- **IMU sample rate is much lower than configured, with host-only
  timestamps.** `camera_info.yaml` requests 800Hz (accel) / 1000Hz (gyro),
  but this hardware has no working hardware trigger for buffered IIO
  capture (`iio:device0`/`iio:device1` have no `trigger/` interface at
  all -- likely the sensor's interrupt line isn't wired in the device
  tree), so `imu_recorder.py` polls the `raw` sysfs attribute directly
  instead. Confirmed achieved rate on real hardware: ~80-95Hz, not 800Hz,
  bounded by Python loop + sysfs read overhead rather than the sensor's own
  clocking. Every sample's `host_timestamp_ns` is the host's own clock at
  read time (`timestamp_source` is always `"poll"`) -- there is no
  hardware-precision timestamp available in this mode at all (the
  `timestamp` channel's attrs are empty outside buffered mode). Fixing this
  properly would mean getting a real hardware trigger working (device-tree
  work to wire up the interrupt line, or finding/building a software timer
  trigger for this specific kernel), which was out of scope this summer.

- **RGB frame drops are elevated when all 5 streams (4 RGB + event + 2 IMU
  threads) run together** -- confirmed ~45-50% drop rate on a full
  `record.py` run, vs. ~20-23% for RGB-only recordings. CPU contention
  (Bayer demosaic + PNG encode for 4 cameras, done in Python, is expensive)
  -- not investigated further/fixed this summer. If this matters for your
  use case, likely angles: move PNG encoding off the grab thread onto a
  separate writer thread/process, reduce resolution/fps, or profile where
  the CPU is actually going.

- **First ~20-60 seconds of any multi-camera recording carries extra
  timestamp noise** while the system reaches steady CPU load (confirmed via
  `analyze_latency.py`'s latency-over-time plot showing a rising-then-
  flattening latency floor early in longer recordings, consistent across
  independent runs) -- not a clock-drift bug (that part IS fixed and stays
  flat for the rest of the recording), just a real system warm-up effect.
  Practical mitigation: let the cameras run for ~30-60s before the portion
  of the recording you actually care about, or trim that window in
  post-processing.

- **Event camera duplicate serial**: `discover_cameras.py` reports **two**
  event cameras with the identical serial string, even though only one is
  physically wired. This is expected, root-caused, and handled in software
  (`Event2`'s serial is left blank in `camera_info.yaml`, and every script
  skips any event camera slot with no serial configured) -- see
  `README.md`'s "Why do two event cameras show the same identifier?"
  section for the full device-tree-level explanation and why it was
  deliberately left as-is rather than patched at the kernel level.

- **`--bias-file` (event camera sensor-level tuning) is not implemented.**
  The originally-guessed API doesn't exist on the installed SDK; the likely
  real path is `device.get_i_ll_biases()`, unconfirmed against real
  hardware. Raises `NotImplementedError` on purpose rather than silently
  guessing. §7 covers the visualization-side filtering that IS available
  today as the practical alternative.

- **Metavision Studio (the proprietary GUI with a bias-tuning panel) will
  not run on this hardware** -- confirmed it's amd64/Ubuntu-only (this is
  aarch64/Jetson), closed-source (so no ARM build is possible even in
  principle), and gated behind a Prophesee customer account. Don't spend
  time chasing this path; `metavision_viewer` (the open-source SDK's
  bundled viewer) is the GUI option that's actually available here, and it
  doesn't have bias controls either -- programmatic tuning via
  `device.get_i_ll_biases()` (see previous bullet, still unimplemented) is
  the only real path to sensor-level tuning on this hardware.

---

## 9. Troubleshooting quick reference

**`VIDIOC_REQBUFS failed: Device or resource busy` (event camera):**
Something else still has the camera's device node open -- usually a
previous script that didn't exit cleanly, or `metavision_viewer` left
running. `ps aux | grep -iE "python|metavision"` and kill any stale
process; `sudo fuser -v /dev/video*` (or `lsof`) to see exactly what's
holding it if nothing obvious shows up in `ps`.

**`PermissionError` from the IMU (`sampling_frequency`, buffer creation, or
plain reads):** Almost always the udev rule from §1.3 not being applied yet
(or `/etc/udev/rules.d/99-iio-permissions.rules` missing after a
reflash) -- re-run those three commands, then confirm with
`ls -la /dev/iio:device0` (should show `crw-rw---- root i2c`, not
`root root`) and `ls -la /sys/bus/iio/devices/iio:device0/buffer/` (should
show group `i2c` with write bits, not `root root`). A `sampling_frequency`
`PermissionError` specifically is treated as non-fatal by `imu_recorder.py`
(prints a warning, continues at the driver's default rate) since it's not
essential -- but a `PermissionError` on buffer creation IS fatal, since
that's required for the wake-up mechanism IMU reads depend on (§1.4).

**IMU readings are frozen (every sample identical, doesn't respond to
moving the sensor):** The channel-enable/buffer-enable wake-up mechanism
described in §1.4/§2 isn't happening -- confirm by reading
`/sys/bus/iio/devices/iio:device0/in_accel_z_raw` twice with a physical
tilt in between, directly via `cat`, outside of any Python code. If it's
genuinely frozen even after `echo 1 > .../scan_elements/in_accel_x_en`
(and y, z) and `echo 1 > .../buffer/enable`, something deeper has changed
(possibly the kernel driver itself, §1.4) -- if it un-freezes after those
manual writes, the bug is in `imu_recorder.py` not doing the same thing
(check its `open()` still calls `channel.enabled = True` for each channel
and still constructs `iio.Buffer(...)`, even though the buffer's *data*
path is never read from).

**`Error TimeHigh discrepancy` / `AssertionError: events['t'][0] (...) prev_ts (...)`
when decoding an `events.raw` file:** The recording itself is corrupted at
the byte level (confirmed: the SDK's own decoder hit a timestamp that goes
backward, its own internal invariant). Not fixable in software -- re-record
rather than trying to salvage the file. `visualize_events.py` will save
whatever was successfully decoded before the corruption point rather than
losing everything, but treat the recording as unreliable overall.

**A `git pull` leaves a file mysteriously empty (0 bytes) despite the pull
reporting success:** Confirmed once this session to be a VS Code
Remote-SSH file-sync race, not a git problem -- `git diff HEAD -- <file>`
will show the working-tree copy as an empty blob (`e69de29`) against a
real, non-empty commit, meaning the checkout itself succeeded but something
(likely an editor buffer) overwrote the file locally afterward. Fix: copy
the correct content back in (from GitHub's web view, or by checking out
the file fresh: `git checkout HEAD -- <path>`), confirm with `wc -l
<file>` that it's no longer 0 lines, then re-`git add` before continuing.

**Video won't play in VS Code's built-in preview ("video format is not
supported"):** See §6.1 -- should be automatically fixed by the H.264
transcode `visualize_dataset.py` does. If you're seeing this on a video
produced by an OLDER run before that fix existed, just re-run
`visualize_dataset.py` on the same `--input-dir`. If it's still happening
on freshly-generated output, run `ffprobe -v error -select_streams v:0
-show_entries stream=codec_name -of csv=p=0 <file>.mp4` to confirm what
codec is actually inside the file, and check the script's console output
for a `WARNING: ffmpeg H.264 transcode failed` or `ffmpeg not found on
PATH` line.

**A camera/serial you expect to see doesn't show up in
`discover_cameras.py`:** For Baslers, check the camera is on the same
subnet as the Jetson's GigE interface (a Basler with a mismatched IP will
silently not respond to discovery) -- `pylonviewer` (from
`/opt/pylon/bin/pylonviewer`) is useful for a lower-level look, and
`PylonGigEConfigurator` (used once this summer to fix exactly this) can
reconfigure a camera's IP, but **be careful with it** -- `auto-ip` (even
with `--dry-run`) has been observed to disrupt active SSH connections on
this network; confirm you have a recoverable path back onto the Jetson
(physical console, or a second SSH session you're prepared to lose) before
running it.

---

## 10. Git workflow used during development

This repo is set up with (at least) two remotes -- check `git remote -v`
for the current names/URLs on your clone, but during this internship they
were `origin` (the NTNU GitLab instance, `git.ntnu.no`) and `github` (a
GitHub mirror). The pattern used throughout this internship to keep both in
sync while developing on `feature/camera_base`:

```bash
git fetch github
git checkout feature/camera_base
git merge github/<branch-with-the-changes-you-want>
git push origin feature/camera_base
git push github feature/camera_base
```

**Check with the repo owner which branch is the current authoritative one
to build from** before starting new work -- `feature/camera_base` was the
active development branch as of this handoff, but branch names and
merge-back status change over time and this document won't stay
up-to-date with that.

If you hit a merge conflict specifically because a local file has
uncommitted changes that duplicate what's already in the commit you're
merging (confirmed to happen once this session after a manual copy-paste
fix, see §9's git troubleshooting entry above), it's usually safe to
discard the local copy and let the merge bring in the real version:
`git restore --staged --worktree <path>` (matching content, so nothing is
actually lost) then retry the merge. Don't do this reflexively though --
check `git diff` first if you're not sure the local changes are truly
redundant with what's incoming.

`config/camera_info.yaml` is intentionally **never** committed (it's
machine/hardware-specific) -- expect `git status` to always show it as
modified on a real Jetson checkout; that's normal, not something to `git
add`.
