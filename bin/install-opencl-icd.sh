#!/bin/bash
# Installs an OpenCL vendor driver (ICD) for the host GPU. Runtime only:
# first_run.sh calls this on the deployment target, while the build needs
# nothing but the Zivid headers and libraries.
#
# The Zivid SDK initialises OpenCL in the Zivid::Application constructor, so a
# machine with no vendor driver aborts the module process before it can
# register viam:zivid:camera:
#
#   An OpenCL error occurred: Failed to get platforms [CL_PLATFORM_NOT_FOUND_KHR]
#
# ocl-icd-libopencl1 on its own does not prevent that — it is the ICD *loader*,
# a dispatch shim that finds real drivers through /etc/OpenCL/vendors/*.icd.
# Neither does clinfo, which is a diagnostic tool rather than a driver.
set -euo pipefail

# Every failing exit path funnels through here. This script cannot fix every
# machine (see the xe caveat below), and first_run.sh deliberately ignores its
# exit code, so a legible hand-off to the operator is the real product of a
# failure.
on_exit() {
    local rc="$1"
    if [[ "$rc" -eq 0 ]]; then
        exit 0
    fi
    cat >&2 <<'EOF'
install-opencl-icd.sh: this host still has no usable OpenCL platform.
The Zivid SDK will abort at startup until it has one.

Inspect what is registered and what the loader can see:

  ls /etc/OpenCL/vendors/     # needs to contain a vendor .icd file
  clinfo -l                   # needs to list at least one platform

Then install the driver matching the GPU (see README, "Linux runtime
prerequisites"):

  sudo apt-get install -y intel-opencl-icd   # Intel
  sudo apt-get install -y mesa-opencl-icd    # AMD, open source stack
  # NVIDIA ships its ICD inside the proprietary driver; there is no apt package

If a vendor package is installed and clinfo still lists no platform, check
which kernel driver owns the GPU:

  ls -l /sys/class/drm/card*/device/driver

Ubuntu 24.04's intel-opencl-icd (23.43) enumerates zero devices on GPUs bound
to the newer xe driver; it works on i915. Intel's PPA
(ppa:kobuk-team/intel-graphics) carries a build that handles xe.
See https://github.com/intel/compute-runtime/issues/903.
EOF
    exit "$rc"
}
trap 'on_exit "$?"' EXIT

# The loader reads nothing but /etc/OpenCL/vendors/*.icd, and an empty file
# there leaves it with no library path to dlopen, so it does not count as a
# registered driver.
icd_registered() {
    local icd
    for icd in /etc/OpenCL/vendors/*.icd; do
        if [[ -s "$icd" ]]; then
            return 0
        fi
    done
    return 1
}

if icd_registered; then
    echo "OpenCL vendor driver already registered:"
    ls /etc/OpenCL/vendors/*.icd
    exit 0
fi

if [[ ! -f /etc/os-release ]]; then
    echo "install-opencl-icd.sh: cannot detect distribution (missing /etc/os-release), skipping" >&2
    exit 0
fi
# shellcheck disable=SC1091
. /etc/os-release

# Unlike the Zivid .deb, the ICD packages below are native to both distributions
# and carry the same names on every release still in support, so the family is
# all that needs matching here.
case "${ID:-}" in
    ubuntu|debian) ;;
    *)
        echo "install-opencl-icd.sh: unsupported distribution ${ID:-unknown}/${VERSION_ID:-unknown}, install an OpenCL vendor driver manually" >&2
        exit 0
        ;;
esac

# Read the PCI bus straight out of sysfs rather than shelling out to lspci:
# pciutils is not guaranteed on a freshly provisioned machine, and pulling it
# in just to name the GPU would be an apt round trip of its own.
HAS_INTEL_GPU=0
HAS_AMD_GPU=0
HAS_NVIDIA_GPU=0
for PCI_DEV in /sys/bus/pci/devices/*; do
    [[ -r "$PCI_DEV/class" && -r "$PCI_DEV/vendor" ]] || continue
    read -r PCI_CLASS <"$PCI_DEV/class"
    # PCI base class 0x03 covers VGA, 3D and "other display" controllers.
    [[ "$PCI_CLASS" == 0x03* ]] || continue
    read -r PCI_VENDOR <"$PCI_DEV/vendor"
    case "$PCI_VENDOR" in
        0x8086) HAS_INTEL_GPU=1 ;;  # Intel
        0x1002) HAS_AMD_GPU=1 ;;    # AMD/ATI
        0x10de) HAS_NVIDIA_GPU=1 ;; # NVIDIA
    esac
done

# Getting this far means nothing is registered under /etc/OpenCL/vendors, so an
# NVIDIA driver — which would have dropped its own .icd — is not in play even on
# a hybrid machine. Prefer whichever GPU apt can actually supply a driver for.
if (( HAS_INTEL_GPU )); then
    ICD_PACKAGE="intel-opencl-icd"
elif (( HAS_AMD_GPU )); then
    ICD_PACKAGE="mesa-opencl-icd"
elif (( HAS_NVIDIA_GPU )); then
    echo "install-opencl-icd.sh: NVIDIA GPU found, and its OpenCL ICD ships with the proprietary driver rather than as a standalone package. Install the NVIDIA driver to get one." >&2
    exit 0
else
    echo "install-opencl-icd.sh: no Intel, AMD or NVIDIA display controller on the PCI bus, install an OpenCL vendor driver manually" >&2
    exit 0
fi

echo "Installing OpenCL driver ${ICD_PACKAGE} and the ocl-icd-libopencl1 loader..."
sudo apt-get update
# Recommends stay enabled here, unlike the SDK install: this mirrors the manual
# procedure in the README, which is the one verified on deployment hardware.
sudo apt-get install -y ocl-icd-libopencl1 "$ICD_PACKAGE"

if ! icd_registered; then
    echo "install-opencl-icd.sh: ${ICD_PACKAGE} installed but no .icd file appeared in /etc/OpenCL/vendors" >&2
    exit 1
fi

echo "OpenCL vendor driver registered:"
ls /etc/OpenCL/vendors/*.icd

# clinfo is a diagnostic rather than a dependency, so verify with it only when
# the machine happens to have it. A registered .icd that still enumerates
# nothing is the xe-driver case the exit handler describes.
if ! command -v clinfo >/dev/null 2>&1; then
    echo "install-opencl-icd.sh: clinfo not installed, skipping the platform check (apt-get install -y clinfo to verify by hand)" >&2
    exit 0
fi

PLATFORMS="$(clinfo --list 2>&1 || true)"
if ! grep -q '^Platform' <<<"$PLATFORMS"; then
    echo "install-opencl-icd.sh: ${ICD_PACKAGE} is installed but clinfo reports no OpenCL platform:" >&2
    echo "$PLATFORMS" >&2
    exit 1
fi

echo "OpenCL platforms:"
echo "$PLATFORMS"
echo "OpenCL driver install complete."
