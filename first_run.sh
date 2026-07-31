#!/bin/bash
# Installs the runtime prerequisites for viam-camera-zivid: an OpenCL vendor
# driver for the host GPU, the Zivid SDK, and the Zivid tools package that
# carries ZividFirmwareUpdater.
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

# The SDK step is the only fatal one: without it the module has nothing to
# link against. It must not end first_run, though, because the steps after it
# apply to a machine that already has the SDK just as much as to a fresh one.
if dpkg -s zivid >/dev/null 2>&1; then
    echo "Zivid SDK already installed."
elif [[ -z "${ZIVID_SDK_RELEASE:-}" ]]; then
    cat >&2 <<EOF
first_run.sh: Zivid SDK not installed and no release pinned.

Set the following environment variable before running this script (find the
current release identifier on https://www.zivid.com/downloads):

  ZIVID_SDK_RELEASE=<version+hash>   # e.g. 2.17.2+440b2367-1

Or install the Zivid debian package manually before running the module.
EOF
    exit 1
else
    export ZIVID_SDK_RELEASE
    "${SCRIPT_DIR}/install-zivid-sdk.sh"
fi

# Tools come last: they pin to whichever SDK release the machine ended up with,
# so they need the step above to have settled. Their failure is not fatal —
# the module captures frames without ZividFirmwareUpdater, and a camera left
# behind by an SDK upgrade is a recovery job that needs the module up and its
# logs readable before anyone can act on it. Guarding the exit code here is
# what makes that true: set -e would abort first_run otherwise, and only a
# separate process can fail without taking this one down with it.
if ! "${SCRIPT_DIR}/install-zivid-tools.sh"; then
    echo "first_run.sh: Zivid tools setup did not complete, the module will start without ZividFirmwareUpdater." >&2
fi
