#!/bin/bash
# Installs the Zivid tools debian package, which carries ZividFirmwareUpdater.
# Runtime only: first_run.sh calls this on the deployment target. The build
# needs headers and shared libraries, and this is a 34 MB download of CLI
# programs that never link into the module, so bin/setup.sh leaves it alone.
#
# Camera firmware is version-locked to the SDK, and the SDK refuses the
# connection outright rather than degrading:
#
#   Unable to connect to camera. Camera 26179B29 requires a firmware update.
#   Camera has firmware "1.36.0+75fc1b59". API supported firmware: "1.39.0+6512a195".
#
# A camera on the wrong firmware is a dead resource until it is reflashed, and
# the firmware image ships inside these packages, so the tools release has to
# be the one the SDK on this machine came from.
set -euo pipefail

if dpkg -s zivid-tools >/dev/null 2>&1; then
    echo "Zivid tools already installed."
    exit 0
fi

# shellcheck source=bin/zivid-deb-target.sh
. "$(dirname "$0")/zivid-deb-target.sh"
# A host the mapping does not cover still runs the module; it just cannot be
# handed a firmware updater from here.
if ! zivid_deb_target "install-zivid-tools.sh"; then
    echo "install-zivid-tools.sh: skipping, install ZividFirmwareUpdater by hand if this machine needs one (see README, \"Camera firmware updates\")" >&2
    exit 0
fi

if [[ "$INSTALL_MODE" != "apt" ]]; then
    echo "install-zivid-tools.sh: this host takes the extract install path rather than apt, which makes it a build agent rather than a machine with a camera on it, skipping" >&2
    exit 0
fi

# Pin to the SDK that is on this machine rather than to ZIVID_SDK_RELEASE:
# first_run leaves an already-installed SDK alone, so the release named in the
# module config can be one this host never took, and a mismatched updater would
# flash firmware the installed SDK then refuses. Fall back to the pinned
# release only when dpkg has no record, which is the run that just installed it.
TOOLS_RELEASE="$(dpkg-query --show --showformat='${Version}' zivid 2>/dev/null || true)"
if [[ -z "$TOOLS_RELEASE" ]]; then
    TOOLS_RELEASE="${ZIVID_SDK_RELEASE:-}"
fi
if [[ -z "$TOOLS_RELEASE" ]]; then
    echo "install-zivid-tools.sh: no zivid package registered with dpkg and ZIVID_SDK_RELEASE is not set, so there is no release to match, skipping" >&2
    exit 0
fi

WORKDIR="$(mktemp -d)"
TOOLS_DEB="zivid-tools_${TOOLS_RELEASE}_${ARCH}.deb"

# zivid-tools is in no apt repo these machines have configured, so a failure
# here leaves an operator with nothing to point at a camera that has already
# stopped working. Hand over the exact commands instead of a bare exit code.
on_exit() {
    local rc="$1"
    rm -rf "$WORKDIR"
    if [[ "$rc" -eq 0 ]]; then
        exit 0
    fi
    cat >&2 <<EOF
install-zivid-tools.sh: could not install zivid-tools ${TOOLS_RELEASE}.
The module runs without it, but a camera whose firmware has fallen behind the
SDK stays unusable until ZividFirmwareUpdater is on this machine.

To install it by hand (the release must match the installed SDK exactly, since
the firmware image ships inside the package):

  curl -fLO https://downloads.zivid.com/sdk/releases/${TOOLS_RELEASE}/${UBUNTU_TAG}/${ARCH_SUBPATH}${TOOLS_DEB}
  sudo apt-get install -y ./${TOOLS_DEB}
EOF
    exit "$rc"
}
trap 'on_exit "$?"' EXIT

echo "Downloading Zivid tools ${TOOLS_RELEASE} (${UBUNTU_TAG}/${ARCH})..."
curl --fail --silent --show-error --location \
    "https://downloads.zivid.com/sdk/releases/${TOOLS_RELEASE}/${UBUNTU_TAG}/${ARCH_SUBPATH}${TOOLS_DEB}" \
    -o "${WORKDIR}/${TOOLS_DEB}"

echo "Installing package via apt..."
sudo apt-get update
sudo apt-get install -y --no-install-recommends "${WORKDIR}/${TOOLS_DEB}"

echo "Zivid tools install complete (run ZividFirmwareUpdater under sudo; see README, \"Camera firmware updates\")."
