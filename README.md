# viam-camera-zivid

A [Viam](https://www.viam.com) module for [Zivid](https://www.zivid.com) 3D cameras.

## Models

### `viam:zivid:camera`

Camera component that streams color images, depth maps, and point clouds from a Zivid camera.

#### Configuration

**Single acquisition:**

```json
{
  "type": "camera",
  "model": "viam:zivid:camera",
  "name": "<name>",
  "attributes": {
    "serial_number": "<serial>",
    "engine": "phase"
  }
}
```

**HDR (multiple acquisitions):**

Acquisition ranges vary by camera model. Use [`get_acquisition_ranges`](#get_acquisition_ranges) to discover valid values for your camera — the numbers below are Zivid 2 defaults and will be rejected by Zivid 3 hardware (e.g. XL250 has a fixed f/3.0 aperture).

```json
{
  "type": "camera",
  "model": "viam:zivid:camera",
  "name": "<name>",
  "attributes": {
    "serial_number": "<serial>",
    "engine": "phase",
    "acquisitions": [
      {"aperture": 5.66, "brightness": 1.8, "exposure_time_us": 1677,  "gain": 1.0},
      {"aperture": 2.83, "brightness": 1.8, "exposure_time_us": 5000,  "gain": 2.0},
      {"aperture": 1.8,  "brightness": 1.8, "exposure_time_us": 20000, "gain": 4.0}
    ]
  }
}
```

#### Attributes

| Name                   | Type   | Required | Description                                                                                                                                 |
| ---------------------- | ------ | -------- | ------------------------------------------------------------------------------------------------------------------------------------------- |
| `serial_number`        | string | No       | Serial number of the camera to connect to. If omitted, connects to the first available camera.                                              |
| `engine`               | string | No       | Zivid Vision Engine to use. Valid values: `phase`, `stripe`, `omni`, `sage`. Default: camera default.                                       |
| `acquisitions`         | list   | No       | List of acquisition configurations. Multiple entries enable HDR capture. Defaults to a single acquisition with camera defaults if omitted.  |
| `pixel_sampling`       | string | No       | 3D (depth / point cloud) resolution. Subsamples or bins the sensor readout — fewer points and **faster capture + processing**. Valid values: `all`, `by2x2`, `by4x4`, `blueSubsample2x2`, `blueSubsample4x4`, `redSubsample2x2`, `redSubsample4x4`. Default: `all` (full resolution). |
| `color_pixel_sampling` | string | No       | 2D color resolution (also the color baked into the point cloud). Same valid values as `pixel_sampling`. Default: `all`.                      |
| `roi`                  | object | No       | Region of interest. See below.                                                                                                              |
| `processing`           | object | No       | Point-cloud processing filters. See below.                                                                                                  |

Each entry in `acquisitions` supports:

| Name               | Type  | Required | Description                                                         |
| ------------------ | ----- | -------- | ------------------------------------------------------------------- |
| `aperture`         | float | No       | Lens aperture as an f-number. Valid range depends on camera model.  |
| `brightness`       | float | No       | Projector brightness. Valid range depends on camera model.          |
| `exposure_time_us` | float | No       | Exposure time in microseconds. Valid range depends on camera model. |
| `gain`             | float | No       | Analog sensor gain. Valid range depends on camera model.            |

#### Config validation

The module validates the config before it starts serving, and refuses to configure rather than
capture with settings that were not asked for:

- **Wrong JSON type** — an attribute written with the wrong type (`"exposure_time_us": "20000"` as a
  string, `"engine": 1` as a number, `acquisitions` as an object instead of a list) is reported by
  full attribute path, e.g. `config attribute 'roi.box.point_o.x' must be of type number, but is of
  type string.`
- **Out-of-range acquisition values** — `aperture`, `brightness`, `exposure_time_us`, `gain` and the
  noise-removal `threshold` are checked twice: against the range the Zivid SDK accepts for any
  camera, then against the range the *connected* camera reports. Values are never silently clamped;
  the error quotes the accepted range and the camera model that rejected the value.
- **Missing required keys** — inside `roi.box`, the keys `point_o`, `point_a`, `point_b` and
  `extents` are required once the block is present.

Use [`get_acquisition_ranges`](#get_acquisition_ranges) to discover the ranges your camera accepts.

#### Region of Interest

Both ROI types are optional and can be used independently or together.

**Depth ROI** — keeps only points whose Z value (distance from camera) falls within the given range (mm):

```json
"roi": {
  "depth": {"min": 500.0, "max": 1500.0}
}
```

**Box ROI** — keeps only points inside a 3D oriented box defined in the camera frame (mm). The box is a parallelepiped formed by an origin point `O` and two edge vectors `OA` and `OB`, extruded along their normal by `extents`:

```json
"roi": {
  "box": {
    "point_o": {"x": -200.0, "y": -150.0, "z": 800.0},
    "point_a": {"x":  200.0, "y": -150.0, "z": 800.0},
    "point_b": {"x": -200.0, "y":  150.0, "z": 800.0},
    "extents": {"min": -200.0, "max": 200.0}
  }
}
```

**Both together:**

```json
"roi": {
  "depth": {"min": 500.0, "max": 1500.0},
  "box": {
    "point_o": {"x": -200.0, "y": -150.0, "z": 800.0},
    "point_a": {"x":  200.0, "y": -150.0, "z": 800.0},
    "point_b": {"x": -200.0, "y":  150.0, "z": 800.0},
    "extents": {"min": -200.0, "max": 200.0}
  }
}
```

All coordinates are in **millimetres** relative to the **camera frame**.

#### Processing filters

Maps to `Zivid::Settings::Processing::Filters`. Only the noise removal filter is exposed today.

**Noise removal** — discards low-confidence points. Raising the threshold removes more noisy/floating points at the cost of leaving holes (missing data); lowering it keeps more data. This is the same control as the "Noise → Removal → Threshold" slider in Zivid Studio. If omitted, the camera's SDK default is used (enabled, threshold ≈ 7).

```json
"processing": {
  "noise_removal": {
    "enabled": true,
    "threshold": 8.0
  }
}
```

#### Resolution and capture time

`pixel_sampling` controls the 3D (depth) resolution by subsampling/binning the sensor readout at acquisition time. Because the camera reads out and processes fewer pixels, lowering it is the most direct way to **speed up captures** — `by2x2` yields roughly 1/4 the points, `by4x4` roughly 1/16.

```json
"pixel_sampling": "by2x2"
```

The point cloud stays fully colored at any resolution — each (now larger) point still carries its color, so it just looks chunkier, never uncolored. `color_pixel_sampling` separately controls the 2D color image resolution; leave it at `all` to keep crisp color while the depth runs at reduced resolution.

> The Zivid SDK validates pixel-sampling combinations and will reject incompatible `pixel_sampling`/`color_pixel_sampling` pairings at capture time. If you hit such an error, set both to the same mode.

| Name        | Type  | Required | Description                                                                                                       |
| ----------- | ----- | -------- | ----------------------------------------------------------------------------------------------------------------- |
| `enabled`   | bool  | No       | Enable/disable the noise removal filter.                                                                          |
| `threshold` | float | No       | Higher = remove more noise (fewer floating points, more holes). Valid range depends on camera; out-of-range values are rejected. |

> Note: noise removal trims floating points but can leave missing data. Per Zivid support, the more robust fix for points "skirting" a vertical surface is to angle the camera (a slight `rx`/`ry` rotation) so the projection isn't grazing the surface.

#### Image sources

`get_images` returns two sources:

| Source name | MIME type            | Description                                                                       |
| ----------- | -------------------- | --------------------------------------------------------------------------------- |
| `color`     | `image/jpeg`         | 2D color image in sRGB color space.                                               |
| `depth`     | `image/vnd.viam.dep` | Depth map with Z values in millimetres as uint16. Invalid points are stored as 0. |

#### Point cloud

`get_point_cloud` returns a binary PCD with `x y z rgb` fields. XYZ values are in **metres**.

#### do_command

##### `get_camera_state`

Returns the current camera state including connection status and temperatures.

```python
result = camera.do_command({"command": "get_camera_state"})
```

Response:

```json
{
  "status": "connected",
  "temperature": {
    "dmd": 45.2,
    "general": 38.1,
    "led": 41.0,
    "lens": 36.5,
    "pcb": 39.8
  }
}
```

`inaccessible_reason` is included only when the camera status is `inaccessible`.

##### `get_acquisition_ranges`

Returns the valid ranges for acquisition parameters for the connected camera model.

```python
result = camera.do_command({"command": "get_acquisition_ranges"})
```

Response:

```json
{
  "aperture":         {"min": 2.37,  "max": 32.0},
  "brightness":       {"min": 1.0,   "max": 2.5},
  "exposure_time_us": {"min": 900,   "max": 100000},
  "gain":             {"min": 1.0,   "max": 16.0}
}
```

Ranges vary by camera model (example above is for a Zivid 2+ MR60). Use this command to discover the valid values for your camera before configuring `acquisitions`.

##### `get_network_configuration`

Returns the camera's IPv4 network configuration.

```python
result = camera.do_command({"command": "get_network_configuration"})
```

Response:

```json
{
  "ipv4": {
    "mode": "dhcp",
    "address": "172.28.60.5",
    "subnet_mask": "255.255.255.0"
  }
}
```

`mode` is either `dhcp` or `manual`. `address` and `subnet_mask` are only meaningful when `mode` is `manual`.

##### `save_zdf`

Captures a single frame with **Zivid diagnostics enabled** and writes it as a `.zdf` file on the machine running the module. Use this to produce a diagnostic capture for Zivid support.

```python
# Default path (/var/lib/viam/zivid_diagnostic_<serial>_<timestamp>.zdf):
result = camera.do_command({"command": "save_zdf"})

# Custom path:
result = camera.do_command({"command": "save_zdf", "path": "/var/lib/viam/issue.zdf"})
```

Arguments:

| Name   | Type   | Required | Description                                                                                          |
| ------ | ------ | -------- | ---------------------------------------------------------------------------------------------------- |
| `path` | string | No       | Absolute path to write the `.zdf` to. Default: `/var/lib/viam/zivid_diagnostic_<serial>_<ms>.zdf`.   |

Response:

```json
{"path": "/var/lib/viam/zivid_diagnostic_<serial>_<ms>.zdf"}
```

Notes:

- The capture is taken with the current configured `acquisitions`/`engine`/`roi` settings, plus `Diagnostics::Enabled=true`, so the resulting file contains the extra data Zivid support needs to investigate issues.
- Diagnostic captures bypass the frame cache and take longer than a normal capture.
- The file is written on the host running `viam-agent` — retrieve it via SSH/SCP (or whatever file access the host allows) and send it to Zivid support.

---

### `viam:zivid:discovery`

Discovery service that enumerates Zivid cameras connected to the machine and returns a `ResourceConfig` for each one that is ready to connect.

#### Configuration

```json
{
  "type": "discovery",
  "model": "viam:zivid:discovery",
  "name": "<name>"
}
```

No attributes required.

#### Discovered resource attributes

Each discovered camera is returned with the following attributes pre-populated:

| Attribute                  | Description                                                                     |
| -------------------------- | ------------------------------------------------------------------------------- |
| `serial_number`            | Camera serial number, ready to paste into a `viam:zivid:camera` config.         |
| `model_name`               | Zivid model name (e.g. `Zivid Two`).                                            |
| `firmware_update_required` | Present and `true` if the camera needs a firmware update before it can be used. |

#### do_command

##### `update_firmware`

Flashes attached Zivid cameras with the firmware required by the Zivid SDK this module was built against. Each SDK version is pinned to a specific camera firmware version; when they disagree the camera reports `firmwareUpdateRequired` and `viam:zivid:camera` cannot connect to it.

Because flashing is destructive and cannot be interrupted safely, the command is a **two-step, confirm-to-run** operation.

**Step 1 — preview (always fails on purpose, changes nothing):**

```python
discovery.do_command({"update_firmware": True})
```

Any value works here — only `{"confirm": true}` runs the update. This call raises an error whose message lists which cameras will be updated, which ones are blocked and why, which are already up to date, and what to do before confirming:

```
update_firmware requires confirmation — NOTHING HAS BEEN CHANGED.

The following 1 camera(s) WILL BE UPDATED to the firmware required by Zivid SDK 2.17.2:
  - Zivid 2+ MR130 S/N 2436D9F1 (firmware 2.15.0, status: firmwareUpdateRequired)

Already up to date (will be left alone):
  - Zivid Two S/N 2308A1B2 (firmware 2.17.2, status: available)

Before confirming:
  1. Remove or disable every viam:zivid:camera component configured for the camera(s) to be updated, ...
  ...

To proceed, send:
  {"update_firmware": {"confirm": true}}
```

**Step 2 — run it:**

```python
discovery.do_command({"update_firmware": {"confirm": True}})
```

Response:

```json
{
  "updated": ["2436D9F1"],
  "skipped": ["2308A1B2: already up to date"],
  "message": "Updated firmware on 1 camera(s). Each updated camera is rebooting — ..."
}
```

If any camera that needed an update was not updated, the command raises an error naming each one and the reason, along with any that did succeed.

**Before running it:**

- **Nothing may hold the camera open.** Remove or disable every `viam:zivid:camera` component configured for the camera's serial number, and close Zivid Studio or any other process using it. A connected or busy camera is reported as blocked rather than flashed.
- **Do not power off or unplug the camera during the update.** Interrupting a firmware update can leave the camera unusable.
- **Expect several minutes per camera.** The call blocks until every camera is done, so use a long client timeout. If the client times out anyway the update keeps running inside the module — progress is logged per stage to the machine logs, and re-running the preview shows the result.
- **The camera reboots afterwards.** Re-add its `viam:zivid:camera` component (or re-run discovery) once it is back.

Concurrent `update_firmware` calls are rejected while an update is running.

---

### `viam:zivid:handeye-calibration`

Generic service that runs Zivid eye-in-hand calibration. Captures detections of a calibration board or ArUco markers from multiple arm poses, then solves for the camera-to-flange transform.

#### Configuration

```json
{
  "type": "generic",
  "model": "viam:zivid:handeye-calibration",
  "name": "<name>",
  "depends_on": ["<arm-name>"],
  "attributes": {
    "arm": "<arm-name>",
    "camera": "<zivid-camera-name>",
    "save_dir": "/var/lib/viam",
    "marker_dictionary": "aruco4x4_50"
  }
}
```

The referenced `viam:zivid:camera` must also exist on the machine; it is looked up by name at runtime (not via `depends_on`), so make sure it is configured and started before the calibration service.

#### Attributes

| Name                | Type   | Required | Description                                                                                                                                |
| ------------------- | ------ | -------- | ------------------------------------------------------------------------------------------------------------------------------------------ |
| `arm`               | string | Yes      | Name of the arm component to read end-effector poses from. Must also appear in `depends_on`.                                               |
| `camera`            | string | Yes      | Name of the `viam:zivid:camera` component to capture frames from.                                                                          |
| `save_dir`          | string | No       | Directory where calibration result JSON files are written. Default: `/var/lib/viam`.                                                       |
| `marker_dictionary` | string | No       | ArUco marker dictionary name (used when `detection_mode='markers'`). Default: `aruco4x4_50`.                                               |

#### do_command

##### `capture_and_detect`

Captures a Zivid frame at the current arm pose and detects either a calibration board or a set of ArUco markers.

```python
# Calibration board (default detection mode):
camera.do_command({"command": "capture_and_detect"})

# Markers:
camera.do_command({
    "command": "capture_and_detect",
    "detection_mode": "markers",
    "marker_ids": [1, 2, 3, 4]
})
```

Arguments:

| Name             | Type        | Required | Description                                                                                       |
| ---------------- | ----------- | -------- | ------------------------------------------------------------------------------------------------- |
| `detection_mode` | string      | No       | `calibration_board` (default) or `markers`.                                                       |
| `marker_ids`     | list of int | Yes\*    | List of ArUco marker IDs to detect. Required when `detection_mode='markers'`.                     |

Response includes `detected` (bool), `accumulated_count`, and either `centroid` (board mode) or `detected_marker_ids` (markers mode). When detection fails, `status_description` explains why.

##### `calibrate_eye_in_hand`

Runs the calibration solver over all accumulated detections.

```python
result = service.do_command({"command": "calibrate_eye_in_hand"})
```

Response (when `valid=true`):

```json
{
  "valid": true,
  "num_inputs": 12,
  "diversity_warning": false,
  "transform": [/* 16 doubles, row-major flange_T_cam */],
  "residuals": [{"rotation_deg": 0.4, "translation_mm": 0.6}, /* ... */],
  "residual_summary": {
    "rotation_mean_deg": 0.5,
    "rotation_max_deg": 1.1,
    "rotation_std_deg": 0.2,
    "translation_mean_mm": 0.7,
    "translation_max_mm": 1.4,
    "translation_std_mm": 0.3
  },
  "quality": "good",
  "frame": {
    "parent": "<arm-name>",
    "translation": {"x": 12.0, "y": -34.0, "z": 56.0},
    "orientation": {"type": "ov_degrees", "value": {"x": 0.0, "y": 0.0, "z": 1.0, "th": 0.0}}
  },
  "output_file": "/var/lib/viam/zivid_handeye_<timestamp>.json"
}
```

`quality` is `good`, `acceptable`, or `poor` based on the mean residuals. `diversity_warning` is `true` when fewer than 10 inputs were used. `frame` is suitable for pasting into a Viam frame-system config.

##### `reset_calibration`

Clears all accumulated detections.

```python
service.do_command({"command": "reset_calibration"})
```

Response: `{"cleared_count": <n>}`.

---

### `viam:zivid:stitcher`

Generic service that captures point clouds from multiple arm poses, transforms each into the arm base frame using a previously computed hand-eye transform, optionally refines alignment with ICP, and exports a merged cloud as PLY (and optionally per-pose PCD).

#### Configuration

```json
{
  "type": "generic",
  "model": "viam:zivid:stitcher",
  "name": "<name>",
  "depends_on": ["<arm-name>"],
  "attributes": {
    "arm": "<arm-name>",
    "camera": "<zivid-camera-name>",
    "hand_eye_json": "/var/lib/viam/zivid_handeye_<timestamp>.json",
    "save_dir": "/var/lib/viam",
    "voxel_size_mm": 1.0,
    "icp_enabled": true,
    "icp_max_correspondence_mm": 2.0,
    "settle_delay_s": 2.0,
    "save_per_pose_clouds": false,
    "scan_poses": [
      {"x": 400, "y":    0, "z": 300, "ox": 0, "oy": 0, "oz": -1, "theta":   0},
      {"x": 400, "y":  100, "z": 300, "ox": 0, "oy": 0, "oz": -1, "theta":  20},
      {"x": 400, "y": -100, "z": 300, "ox": 0, "oy": 0, "oz": -1, "theta": -20}
    ]
  }
}
```

As with the calibration service, the `viam:zivid:camera` is looked up by name at capture time, so it does not need to appear in `depends_on` — but it must be configured and started.

#### Attributes

| Name                        | Type    | Required | Description                                                                                                                       |
| --------------------------- | ------- | -------- | --------------------------------------------------------------------------------------------------------------------------------- |
| `arm`                       | string  | Yes      | Name of the arm component used to move between scan poses. Must also appear in `depends_on`.                                      |
| `camera`                    | string  | Yes      | Name of the `viam:zivid:camera` to capture from.                                                                                  |
| `hand_eye_json`             | string  | Yes      | Path to the JSON file produced by `viam:zivid:handeye-calibration`. Provides the `flange_T_cam` transform.                        |
| `save_dir`                  | string  | No       | Directory for scan outputs. Default: `/var/lib/viam`.                                                                             |
| `voxel_size_mm`             | float   | No       | Voxel-grid downsample size applied to each captured cloud and to the final export. Default: `1.0`.                                |
| `icp_enabled`               | bool    | No       | If `true`, refines pose-based alignment with local ICP after each capture. Default: `true`.                                       |
| `icp_max_correspondence_mm` | float   | No       | Maximum correspondence distance (mm) used by the ICP solver. Default: `2.0`.                                                      |
| `settle_delay_s`            | float   | No       | Seconds to wait after each arm move before capturing. Default: `2.0`.                                                             |
| `save_per_pose_clouds`      | bool    | No       | If `true`, writes a binary PCD for each individual pose alongside the merged PLY. Default: `false`.                               |
| `scan_poses`                | list    | No       | List of arm target poses (`x`, `y`, `z` in mm; `ox`, `oy`, `oz`, `theta` as OV in degrees). Required for `run_scan`.              |

#### do_command

##### `run_scan`

Moves the arm through each configured `scan_poses` entry, captures + accumulates a point cloud at each pose, and exports the merged result.

```python
result = service.do_command({"command": "run_scan"})
```

Response includes `output_dir`, `output_file` (`merged.ply`, in metres), `point_count`, `poses_attempted`, `poses_captured`, and a `pose_results` list with per-pose move/capture status.

##### `capture_and_accumulate`

Captures a single point cloud at the *current* arm pose and adds it to the running accumulated cloud. Useful for ad-hoc scanning without pre-configured `scan_poses`.

```python
service.do_command({"command": "capture_and_accumulate"})
```

Response: `{"accumulated_count": <n>, "point_count": <total points>}`.

##### `export`

Writes the current accumulated cloud to a PLY file (in metres) under `save_dir`.

```python
result = service.do_command({"command": "export"})
```

Response: `{"output_file": "<path>.ply", "point_count": <n>}`.

##### `reset_scan`

Clears the accumulated point cloud and resets the capture counter.

```python
service.do_command({"command": "reset_scan"})
```

Response: `{"cleared_count": <n>}`.

---

## Linux runtime prerequisites

The Zivid SDK uses OpenCL for point-cloud reconstruction, HDR merging, and ROI filtering, and initialises it in the SDK constructor. On a fresh Linux machine no OpenCL vendor driver is installed, and the module aborts at startup before `viam:zivid:camera` can register:

```
terminate called after throwing an instance of 'Zivid::Exceptions::ReturnCode<int>'
  what():  An OpenCL error occurred: Failed to get platforms
Make sure the OpenCL driver is installed and working ('clinfo' needs to show at least one platform).
[CL_PLATFORM_NOT_FOUND_KHR]
```

The module's `first_run` hook handles steps 1 and 2 below automatically on Ubuntu and Debian hosts: `first_run.sh` calls `install-opencl-icd.sh`, which detects the GPU on the PCI bus, installs the matching vendor package, and verifies the result with `clinfo` when it is available. It is idempotent — an already registered driver is left alone — and non-fatal, so a machine it cannot work out is left to the operator with a warning in the module logs rather than a module that refuses to start.

Work through the steps by hand when that warning appears, when `clinfo` reports no platform, or when setting up a machine outside the supported distributions. Step 3 is never automated: it needs a reboot to take effect.

### 1. Install the OpenCL ICD loader

```bash
sudo apt-get update
sudo apt-get install -y ocl-icd-libopencl1 clinfo
```

This is not sufficient on its own. `ocl-icd-libopencl1` is the ICD *loader*, a dispatch shim that finds real drivers through `/etc/OpenCL/vendors/*.icd`, and `clinfo` is a diagnostic tool. Neither one is a driver — step 2 is what makes OpenCL work.

### 2. Install a vendor ICD for your hardware

| Hardware              | Package / source                                                |
| --------------------- | --------------------------------------------------------------- |
| Intel iGPU / CPU      | `intel-opencl-icd`                                              |
| AMD GPU (open source) | `mesa-opencl-icd`                                               |
| AMD GPU (ROCm)        | install ROCm runtime per AMD's documentation                    |
| NVIDIA GPU            | install the proprietary NVIDIA driver — OpenCL ICD is bundled   |

For example, on an Intel host:

```bash
sudo apt-get install -y intel-opencl-icd
```

### 3. Grant GPU access to whoever runs the SDK

`/dev/dri/renderD128` is owned by `root:render`, and on a fresh install the `render` group is empty. Anything that is neither root nor a member of that group gets no OpenCL platform, driver or no driver. Two different users hit this:

- **`viam-agent`**, where it is configured to run as the `viam` user rather than as root.
- **You**, whenever you run `clinfo` or `ZividFirmwareUpdater` from a shell.

```bash
sudo usermod -aG render,video viam    # and/or your own login
sudo reboot
```

A reboot (or at minimum a restart of `viam-agent`) is required for the new group membership to take effect. For one-off interactive commands, `sudo` does the same job with no reboot.

### 4. Verify

```bash
clinfo -l                 # should list at least one platform and device
ls /etc/OpenCL/vendors/   # should list the vendor ICD installed above
ls -l /dev/dri/           # render/card nodes should exist
```

`clinfo` should report at least one OpenCL platform and one device. If it prints `Number of platforms: 0`, the vendor ICD is missing or not registered under `/etc/OpenCL/vendors/`.

> **`Failed to get platforms` / `CL_PLATFORM_NOT_FOUND_KHR` does not necessarily mean the driver is missing.** The ICD loader emits that exact message — the one quoted at the top of this section — when it cannot open `/dev/dri/renderD128`, which is what a non-root user outside the `render` group always gets. Before chasing a driver problem, run the same command again under `sudo`. If it now lists a platform, the driver is fine and step 3 is what is missing.

If `intel-opencl-icd` is installed and registered but `clinfo` still reports no platform even under `sudo`, check which kernel driver owns the GPU:

```bash
ls -l /sys/class/drm/card*/device/driver
```

Ubuntu 24.04's `intel-opencl-icd` (23.43) enumerates zero devices on GPUs bound to the newer `xe` driver; it works on `i915`. Intel's PPA (`ppa:kobuk-team/intel-graphics`) carries a build that handles `xe`. See [intel/compute-runtime#903](https://github.com/intel/compute-runtime/issues/903). The first-run hook does not add that PPA — adding third-party apt sources is an operator decision.

---

## Camera firmware updates

Camera firmware is version-locked to the SDK. The SDK does not degrade or warn when they disagree, it refuses the connection, so a camera left behind by an SDK upgrade is a dead resource until it is reflashed:

```
Unable to connect to camera. Camera 26179B29 requires a firmware update.
Camera has firmware "1.36.0+75fc1b59". API supported firmware: "1.39.0+6512a195".
```

The updater is `ZividFirmwareUpdater`, from the `zivid-tools` package. `first_run.sh` installs it via `install-zivid-tools.sh`, pinned to the release the SDK on that machine actually came from: the firmware image ships inside the package, so a `zivid-tools` that does not match the installed SDK would flash the wrong firmware. The step is idempotent and non-fatal, so confirm it is there rather than assuming:

```bash
dpkg -s zivid-tools | grep -E '^(Status|Version)'
dpkg -s zivid       | grep -E '^(Status|Version)'   # versions must match
```

### Running the updater

Stop the module (or the machine) first — the updater needs to open the camera, and the SDK will not hand it over while the module holds it.

```bash
sudo ZividFirmwareUpdater          # walks the cameras it can see
sudo ZividFirmwareUpdater --help   # non-interactive options
```

> **Run it under `sudo`.** Without it the updater fails on a machine whose OpenCL is perfectly healthy, and it fails with the *same* message as a machine with no driver at all:
>
> ```
> An OpenCL error occurred: Failed to get platforms
> [CL_PLATFORM_NOT_FOUND_KHR]
> ```
>
> `/dev/dri/renderD128` is `root:render` and the `render` group is empty by default, so an ordinary login sees no platform. `sudo clinfo -l` versus `clinfo -l` tells the two cases apart in one step. See [step 3](#3-grant-gpu-access-to-whoever-runs-the-sdk) for the permanent fix.

### Installing `zivid-tools` by hand

`zivid-tools` is not in any apt repo configured on these machines; it is fetched from Zivid's downloads server by release. Match the installed SDK exactly, and match the Ubuntu flavor (`u20`, `u22` or `u24`) and architecture of the host:

```bash
REL=$(dpkg-query --show --showformat='${Version}' zivid)   # e.g. 2.17.2+440b2367-1
curl -fLO "https://downloads.zivid.com/sdk/releases/${REL}/u24/zivid-tools_${REL}_amd64.deb"
sudo apt-get install -y "./zivid-tools_${REL}_amd64.deb"
```

On arm64 the path carries an extra directory: `.../u24/arm64/zivid-tools_${REL}_arm64.deb`.

---

## Building

### Prerequisites

- Zivid SDK 2.x installed (tested with 2.17.2)
- Python 3 (for Conan venv)
- CMake ≥ 3.25
- GCC with C++17 support

### Steps

```bash
make setup   # creates venv, installs Conan, adds Viam Conan remote (run once)
make build   # compiles and packages into module.tar.gz
```

### Clean

```bash
make clean
```
