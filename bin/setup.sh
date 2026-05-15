#!/bin/bash
# Bootstraps a build environment for viam-camera-zivid: installs system
# packages, creates a Python venv, installs Conan, and registers the Viam
# Conan remote so viam-cpp-sdk can be resolved.
#
# Ported from viam-modules/viam-camera-realsense (bin/setup.sh).
set -euxo pipefail

OS=$(uname -s | tr '[:upper:]' '[:lower:]')

if [[ ${OS} == "linux" ]]; then
    sudo apt-get update
    sudo apt-get install -y wget gpg lsb-release

    # The CMakeLists.txt requires CMake >= 3.25; Ubuntu 22.04 ships 3.22.
    # Add the Kitware apt repo so we get a current CMake.
    if lsb_release -is | grep -q "Ubuntu"; then
        sudo wget -O - https://apt.kitware.com/keys/kitware-archive-latest.asc 2>/dev/null \
            | sudo gpg --dearmor - \
            | sudo tee /usr/share/keyrings/kitware-archive-keyring.gpg >/dev/null
        echo "deb [signed-by=/usr/share/keyrings/kitware-archive-keyring.gpg] https://apt.kitware.com/ubuntu/ $(lsb_release -cs) main" \
            | sudo tee /etc/apt/sources.list.d/kitware.list >/dev/null
        sudo apt-get update
    fi

    sudo apt-get install -y \
        autoconf \
        automake \
        build-essential \
        ca-certificates \
        cmake \
        cmake-data \
        curl \
        g++ \
        git \
        gnupg \
        libssl-dev \
        ninja-build \
        pkg-config \
        python3 \
        python3-pip \
        python3-venv \
        software-properties-common
elif [[ ${OS} == "darwin" ]]; then
    if ! command -v brew >/dev/null 2>&1; then
        echo "Homebrew not found. Please install it first: https://brew.sh/" >&2
        exit 1
    fi
    brew install cmake ninja pkg-config python
fi

if [[ ${OS} == "linux" ]]; then
    # Default the SDK release for the build; first_run.sh installs the
    # matching version at runtime on the deployment target. Override by
    # setting ZIVID_SDK_RELEASE in the build environment (find current values
    # on https://www.zivid.com/downloads).
    export ZIVID_SDK_RELEASE="${ZIVID_SDK_RELEASE:-2.17.2+440b2367-1}"
    bin/install-zivid-sdk.sh
fi

if ! command -v python3 >/dev/null 2>&1; then
    echo "python3 not found in PATH. Aborting." >&2
    exit 1
fi

if ! python3 -m venv --help >/dev/null 2>&1; then
    echo "python3 venv module not available. On Ubuntu try: sudo apt-get install --reinstall python3-venv python3-full python3-pip" >&2
    exit 1
fi

if [ ! -f "./venv/bin/activate" ]; then
    echo "creating virtual env"
    python3 -m venv venv
fi
# shellcheck disable=SC1091
source ./venv/bin/activate

if [ ! -f "./venv/bin/conan" ]; then
    echo "installing conan"
    pip install --upgrade pip
    pip install conan
fi

conan profile detect --force
conan remote add viamconan https://viam.jfrog.io/artifactory/api/conan/viamconan --index 0 --force
