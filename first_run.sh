#!/usr/bin/env bash
#
# Installs the Zivid SDK on the host before the Viam module's first run.
# Viam invokes this script once per machine (declared via `first_run` in
# meta.json). The Zivid version here MUST match the version the module was
# linked against in the Dockerfile.
#
set -euo pipefail

ZIVID_VERSION="2.17.2+440b2367-1"
ZIVID_SO="libZividCore.so.${ZIVID_VERSION%%+*}"

if ldconfig -p 2>/dev/null | grep -q "${ZIVID_SO}"; then
    echo "Zivid SDK ${ZIVID_VERSION%%+*} already installed; skipping."
    exit 0
fi

if [[ $EUID -ne 0 ]]; then
    if command -v sudo >/dev/null 2>&1; then
        SUDO="sudo -n"
    else
        echo "ERROR: first_run requires root or passwordless sudo to install Zivid SDK" >&2
        exit 1
    fi
else
    SUDO=""
fi

ARCH="$(dpkg --print-architecture 2>/dev/null || echo unknown)"
if [[ "${ARCH}" != "amd64" ]]; then
    echo "ERROR: Zivid first_run supports only amd64 (got '${ARCH}')" >&2
    exit 1
fi

TMP="$(mktemp -d)"
trap 'rm -rf "${TMP}"' EXIT
cd "${TMP}"

echo "Downloading Zivid SDK ${ZIVID_VERSION}..."
BASE="https://downloads.zivid.com/sdk/releases/${ZIVID_VERSION}/u22/amd64"
for pkg in zivid zivid-tools zivid-genicam; do
    wget -q "${BASE}/${pkg}_${ZIVID_VERSION}_amd64.deb"
done

echo "Installing Zivid SDK..."
${SUDO} apt-get update
${SUDO} apt-get install -y --no-install-recommends ./zivid_${ZIVID_VERSION}_amd64.deb \
                                                   ./zivid-tools_${ZIVID_VERSION}_amd64.deb \
                                                   ./zivid-genicam_${ZIVID_VERSION}_amd64.deb

echo "Zivid SDK ${ZIVID_VERSION%%+*} installed."
