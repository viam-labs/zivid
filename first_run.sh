#!/bin/bash
set -euo pipefail

# Installs the Zivid SDK runtime needed by viam-camera-zivid.
#
# Override this to pin a different SDK build. The full release URL must
# include the git-hash suffix Zivid uses on their downloads server
# (e.g. "2.17.2+90cfdc4b-1"). Find the current value at
# https://www.zivid.com/downloads.
ZIVID_SDK_RELEASE="${ZIVID_SDK_RELEASE:-}"

OS="$(uname -s)"
if [[ "$OS" != "Linux" ]]; then
    echo "first_run.sh: only Linux is supported (got $OS)" >&2
    exit 1
fi

if [[ ! -f /etc/os-release ]]; then
    echo "first_run.sh: cannot detect distribution (missing /etc/os-release)" >&2
    exit 1
fi
# shellcheck disable=SC1091
. /etc/os-release
if [[ "${ID:-}" != "ubuntu" ]]; then
    echo "first_run.sh: only Ubuntu is supported (got ID=${ID:-unknown})" >&2
    exit 1
fi

case "${VERSION_ID:-}" in
    20.04) UBUNTU_TAG="u20" ;;
    22.04) UBUNTU_TAG="u22" ;;
    24.04) UBUNTU_TAG="u24" ;;
    *)
        echo "first_run.sh: unsupported Ubuntu version ${VERSION_ID:-unknown}" >&2
        exit 1
        ;;
esac

ARCH="$(dpkg --print-architecture)"
if [[ "$ARCH" != "amd64" && "$ARCH" != "arm64" ]]; then
    echo "first_run.sh: unsupported architecture $ARCH" >&2
    exit 1
fi

if dpkg -s zivid >/dev/null 2>&1; then
    echo "Zivid SDK already installed."
    exit 0
fi

if [[ -z "$ZIVID_SDK_RELEASE" ]]; then
    cat >&2 <<EOF
first_run.sh: Zivid SDK not installed and no release pinned.

Set the following environment variable before running this script (find the
current release identifier on https://www.zivid.com/downloads):

  ZIVID_SDK_RELEASE=<version+hash>   # e.g. 2.17.2+90cfdc4b-1

Or install the Zivid debian package manually before running the module.
EOF
    exit 1
fi

WORKDIR="$(mktemp -d)"
trap 'rm -rf "$WORKDIR"' EXIT

BASE_URL="https://downloads.zivid.com/sdk/releases"
ZIVID_DEB="zivid_${ZIVID_SDK_RELEASE}_${ARCH}.deb"

echo "Downloading Zivid SDK ${ZIVID_SDK_RELEASE}..."
curl --fail --silent --show-error --location \
    "${BASE_URL}/${ZIVID_SDK_RELEASE}/${UBUNTU_TAG}/${ZIVID_DEB}" \
    -o "${WORKDIR}/${ZIVID_DEB}"

echo "Installing package..."
sudo apt-get update
sudo apt-get install -y --no-install-recommends "${WORKDIR}/${ZIVID_DEB}"

echo "Zivid SDK install complete."
