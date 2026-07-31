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

# shellcheck source=bin/zivid-deb-target.sh
. "$(dirname "$0")/zivid-deb-target.sh"
# Without a package to fetch there is no build and no runtime, so an
# unsupported host is fatal here.
zivid_deb_target "install-zivid-sdk.sh"

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
