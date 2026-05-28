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
    "engine": "omni",
    "acquisitions": [
      {"aperture": 5.66, "brightness": 1.8, "exposure_time_us": 10000, "gain": 1.0}
    ]
  }
}
```

**HDR (multiple acquisitions):**

```json
{
  "type": "camera",
  "model": "viam:zivid:camera",
  "name": "<name>",
  "attributes": {
    "serial_number": "<serial>",
    "engine": "omni",
    "acquisitions": [
      {"aperture": 5.66, "brightness": 1.8, "exposure_time_us": 1677,  "gain": 1.0},
      {"aperture": 2.83, "brightness": 1.8, "exposure_time_us": 5000,  "gain": 2.0},
      {"aperture": 1.8,  "brightness": 1.8, "exposure_time_us": 20000, "gain": 4.0}
    ]
  }
}
```

#### Attributes

| Name            | Type   | Required | Description                                                                                                                                |
| --------------- | ------ | -------- | ------------------------------------------------------------------------------------------------------------------------------------------ |
| `serial_number` | string | No       | Serial number of the camera to connect to. If omitted, connects to the first available camera.                                             |
| `engine`        | string | No       | Zivid Vision Engine to use. Valid values: `phase`, `stripe`, `omni`, `sage`. Default: camera default.                                      |
| `acquisitions`  | list   | No       | List of acquisition configurations. Multiple entries enable HDR capture. Defaults to a single acquisition with camera defaults if omitted. |
| `roi`           | object | No       | Region of interest. See below.                                                                                                             |

Each entry in `acquisitions` supports:

| Name               | Type  | Required | Description                                                         |
| ------------------ | ----- | -------- | ------------------------------------------------------------------- |
| `aperture`         | float | No       | Lens aperture as an f-number. Valid range depends on camera model.  |
| `brightness`       | float | No       | Projector brightness. Valid range depends on camera model.          |
| `exposure_time_us` | float | No       | Exposure time in microseconds. Valid range depends on camera model. |
| `gain`             | float | No       | Analog sensor gain. Valid range depends on camera model.            |

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

The Zivid SDK uses OpenCL for point-cloud reconstruction, HDR merging, and ROI filtering. On a fresh Linux machine OpenCL is not installed by default and the module will fail to capture frames until the runtime is set up.

### 1. Install the OpenCL ICD loader

```bash
sudo apt-get update
sudo apt-get install -y ocl-icd-libopencl1 clinfo
```

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

### 3. Grant the `viam` user GPU access

`viam-agent` runs as the `viam` user, which must be in the `render` and `video` groups to access `/dev/dri/*`:

```bash
sudo usermod -aG render,video viam
sudo reboot
```

A reboot (or at minimum a restart of `viam-agent`) is required for the new group membership to take effect.

### 4. Verify

```bash
clinfo | head -20
ls /etc/OpenCL/vendors/   # should list the vendor ICD installed above
ls -l /dev/dri/           # render/card nodes should exist
```

`clinfo` should report at least one OpenCL platform and one device. If it prints `Number of platforms: 0`, the vendor ICD is missing or not registered under `/etc/OpenCL/vendors/`.

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
