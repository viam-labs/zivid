#!/bin/bash
# Installs the Zivid SDK debian package. Shared by bin/setup.sh (build time)
# and first_run.sh (runtime on the deployment target).
#
# Requires ZIVID_SDK_RELEASE to be set to a release identifier including the
# git-hash suffix Zivid uses on their downloads server, e.g.
# "2.17.2+440b2367-1". Find current values at https://www.zivid.com/downloads.
set -euo pipefail

if dpkg -s zivid >/dev/null 2>&1; then
    echo "Zivid SDK already installed."
    exit 0
fi

if [[ -z "${ZIVID_SDK_RELEASE:-}" ]]; then
    echo "install-zivid-sdk.sh: ZIVID_SDK_RELEASE is not set" >&2
    exit 1
fi

OS="$(uname -s)"
if [[ "$OS" != "Linux" ]]; then
    echo "install-zivid-sdk.sh: only Linux is supported (got $OS)" >&2
    exit 1
fi

if [[ ! -f /etc/os-release ]]; then
    echo "install-zivid-sdk.sh: cannot detect distribution (missing /etc/os-release)" >&2
    exit 1
fi
# shellcheck disable=SC1091
. /etc/os-release
if [[ "${ID:-}" != "ubuntu" ]]; then
    echo "install-zivid-sdk.sh: only Ubuntu is supported (got ID=${ID:-unknown})" >&2
    exit 1
fi

case "${VERSION_ID:-}" in
    20.04) UBUNTU_TAG="u20" ;;
    22.04) UBUNTU_TAG="u22" ;;
    24.04) UBUNTU_TAG="u24" ;;
    *)
        echo "install-zivid-sdk.sh: unsupported Ubuntu version ${VERSION_ID:-unknown}" >&2
        exit 1
        ;;
esac

ARCH="$(dpkg --print-architecture)"
if [[ "$ARCH" != "amd64" && "$ARCH" != "arm64" ]]; then
    echo "install-zivid-sdk.sh: unsupported architecture $ARCH" >&2
    exit 1
fi

WORKDIR="$(mktemp -d)"
trap 'rm -rf "$WORKDIR"' EXIT
ZIVID_DEB="zivid_${ZIVID_SDK_RELEASE}_${ARCH}.deb"

echo "Downloading Zivid SDK ${ZIVID_SDK_RELEASE} (${UBUNTU_TAG}/${ARCH})..."
curl --fail --silent --show-error --location \
    "https://downloads.zivid.com/sdk/releases/${ZIVID_SDK_RELEASE}/${UBUNTU_TAG}/${ZIVID_DEB}" \
    -o "${WORKDIR}/${ZIVID_DEB}"

echo "Installing package..."
sudo apt-get update
sudo apt-get install -y --no-install-recommends "${WORKDIR}/${ZIVID_DEB}"
echo "Zivid SDK install complete."
