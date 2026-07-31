#!/bin/bash
# Installs the runtime prerequisites for viam-camera-zivid: an OpenCL vendor
# driver for the host GPU, then the Zivid SDK itself.
set -euo pipefail

SCRIPT_DIR="$(dirname "$0")"

# OpenCL comes first, and its failure is not fatal. It runs ahead of the SDK
# steps because those return early on a machine that already has the SDK and
# bail out when no release is pinned, and the module aborts at startup without
# an OpenCL driver regardless of how the SDK got there. A machine that has been
# provisioned some other way, or that needs a driver this script cannot install,
# is better served by the module starting and the operator reading the warning
# than by first_run failing outright.
if ! "${SCRIPT_DIR}/install-opencl-icd.sh"; then
    echo "first_run.sh: OpenCL driver setup did not complete, continuing with the Zivid SDK install." >&2
fi

if dpkg -s zivid >/dev/null 2>&1; then
    echo "Zivid SDK already installed."
    exit 0
fi

if [[ -z "${ZIVID_SDK_RELEASE:-}" ]]; then
    cat >&2 <<EOF
first_run.sh: Zivid SDK not installed and no release pinned.

Set the following environment variable before running this script (find the
current release identifier on https://www.zivid.com/downloads):

  ZIVID_SDK_RELEASE=<version+hash>   # e.g. 2.17.2+440b2367-1

Or install the Zivid debian package manually before running the module.
EOF
    exit 1
fi

export ZIVID_SDK_RELEASE
exec "${SCRIPT_DIR}/install-zivid-sdk.sh"
