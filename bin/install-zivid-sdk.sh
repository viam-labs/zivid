#!/bin/bash
# Installs the Zivid SDK debian package. Shared by bin/setup.sh (build time)
# and first_run.sh (runtime on the deployment target).
#
# Requires ZIVID_SDK_RELEASE to be set to a release identifier including the
# git-hash suffix Zivid uses on their downloads server, e.g.
# "2.17.2+440b2367-1". Find current values at https://www.zivid.com/downloads.
#
# Zivid publishes .deb packages targeting Ubuntu 20.04 / 22.04 / 24.04. On
# Ubuntu hosts we install via apt so dpkg state and udev rules are set up
# correctly. On Debian hosts (e.g. the Viam cloud build agent runs Debian
# bullseye/bookworm) we pick the Ubuntu .deb whose glibc generation matches
# and extract the files with dpkg-deb -x: that gives the build the headers
# and shared libraries without inviting apt to resolve Ubuntu-only deps
# against Debian's package set.
set -euo pipefail

# Either a registered package or the headers on disk (extracted path) counts
# as "already installed" — the build only needs the files in /usr.
if dpkg -s zivid >/dev/null 2>&1 || [[ -f /usr/include/Zivid/Application.h ]]; then
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

# Map (distro, version) → (Ubuntu .deb flavor, install mode). For Debian we
# pick the closest glibc match: bullseye (2.31) ≈ Ubuntu 20.04, bookworm
# (2.36) ≈ Ubuntu 22.04.
case "${ID:-},${VERSION_ID:-}" in
    ubuntu,20.04) UBUNTU_TAG="u20"; INSTALL_MODE="apt" ;;
    ubuntu,22.04) UBUNTU_TAG="u22"; INSTALL_MODE="apt" ;;
    ubuntu,24.04) UBUNTU_TAG="u24"; INSTALL_MODE="apt" ;;
    debian,11)    UBUNTU_TAG="u20"; INSTALL_MODE="extract" ;;
    debian,12)    UBUNTU_TAG="u22"; INSTALL_MODE="extract" ;;
    *)
        echo "install-zivid-sdk.sh: unsupported distribution ${ID:-unknown}/${VERSION_ID:-unknown}" >&2
        exit 1
        ;;
esac

ARCH="$(dpkg --print-architecture)"
case "$ARCH" in
    amd64) ARCH_SUBPATH="" ;;
    # Zivid hosts arm64 .deb's one directory deeper than amd64
    # (e.g. .../u22/arm64/zivid_<rel>_arm64.deb).
    arm64) ARCH_SUBPATH="arm64/" ;;
    *)
        echo "install-zivid-sdk.sh: unsupported architecture $ARCH" >&2
        exit 1
        ;;
esac

WORKDIR="$(mktemp -d)"
trap 'rm -rf "$WORKDIR"' EXIT
ZIVID_DEB="zivid_${ZIVID_SDK_RELEASE}_${ARCH}.deb"

echo "Downloading Zivid SDK ${ZIVID_SDK_RELEASE} (${UBUNTU_TAG}/${ARCH})..."
curl --fail --silent --show-error --location \
    "https://downloads.zivid.com/sdk/releases/${ZIVID_SDK_RELEASE}/${UBUNTU_TAG}/${ARCH_SUBPATH}${ZIVID_DEB}" \
    -o "${WORKDIR}/${ZIVID_DEB}"

case "$INSTALL_MODE" in
    apt)
        echo "Installing package via apt..."
        sudo apt-get update
        sudo apt-get install -y --no-install-recommends "${WORKDIR}/${ZIVID_DEB}"
        ;;
    extract)
        echo "Extracting package contents (Ubuntu .deb on Debian, no dpkg state)..."
        # Recent Ubuntu .deb's compress data.tar with zstd, which dpkg-deb
        # on Debian bullseye (dpkg 1.20) can't read. Unpack the outer ar
        # archive ourselves and let tar handle the inner payload — that
        # works regardless of whether it's .zst, .xz, or .gz.
        sudo apt-get install -y --no-install-recommends zstd
        UNPACK_DIR="${WORKDIR}/unpack"
        mkdir -p "${UNPACK_DIR}"
        ( cd "${UNPACK_DIR}" && ar x "${WORKDIR}/${ZIVID_DEB}" )
        DATA_TAR=$(ls "${UNPACK_DIR}"/data.tar.* | head -n1)
        sudo tar -axf "${DATA_TAR}" -C /
        sudo ldconfig
        ;;
esac

echo "Zivid SDK install complete."
