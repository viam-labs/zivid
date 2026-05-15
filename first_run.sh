#!/bin/bash
# Installs the Zivid SDK runtime needed by viam-camera-zivid.
set -euo pipefail

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
exec "$(dirname "$0")/install-zivid-sdk.sh"
