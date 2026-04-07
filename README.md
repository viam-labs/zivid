# viam-camera-zivid

A [Viam](https://www.viam.com) module for [Zivid](https://www.zivid.com) 3D cameras.

## Models

### `viam:camera:zivid`

Camera component that streams color images, depth maps, and point clouds from a Zivid camera.

#### Configuration

**Single acquisition:**

```json
{
  "type": "camera",
  "model": "viam:camera:zivid",
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
  "model": "viam:camera:zivid",
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

| Name | Type | Required | Description |
|---|---|---|---|
| `serial_number` | string | No | Serial number of the camera to connect to. If omitted, connects to the first available camera. |
| `engine` | string | No | Zivid Vision Engine to use. Valid values: `phase`, `stripe`, `omni`, `sage`. Default: camera default. |
| `acquisitions` | list | No | List of acquisition configurations. Multiple entries enable HDR capture. Defaults to a single acquisition with camera defaults if omitted. |
| `roi` | object | No | Region of interest. See below. |

Each entry in `acquisitions` supports:

| Name | Type | Required | Description |
|---|---|---|---|
| `aperture` | float | No | Lens aperture as an f-number. Valid range depends on camera model. |
| `brightness` | float | No | Projector brightness. Valid range depends on camera model. |
| `exposure_time_us` | float | No | Exposure time in microseconds. Valid range depends on camera model. |
| `gain` | float | No | Analog sensor gain. Valid range depends on camera model. |

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

| Source name | MIME type | Description |
|---|---|---|
| `color` | `image/jpeg` | 2D color image in sRGB color space. |
| `depth` | `image/vnd.viam.dep` | Depth map with Z values in millimetres as uint16. Invalid points are stored as 0. |

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

| Attribute | Description |
|---|---|
| `serial_number` | Camera serial number, ready to paste into a `viam:camera:zivid` config. |
| `model_name` | Zivid model name (e.g. `Zivid Two`). |
| `firmware_update_required` | Present and `true` if the camera needs a firmware update before it can be used. |

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
