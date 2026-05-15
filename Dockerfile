FROM ubuntu:jammy

ARG DEBIAN_FRONTEND=noninteractive

# Zivid SDK version. Bump this string when upgrading.
ARG ZIVID_VERSION=2.17.2+440b2367-1

# Viam C++ SDK version. Must match the requirement in conanfile.py.
ARG VIAM_CPP_SDK_VERSION=0.33.1

RUN apt-get update && \
    apt-get -y dist-upgrade && \
    apt-get -y --no-install-recommends install \
        build-essential \
        ca-certificates \
        ccache \
        curl \
        g++ \
        git \
        gnupg \
        gpg \
        jq \
        lsb-release \
        ninja-build \
        pkg-config \
        python3 \
        python3-pip \
        python3-venv \
        software-properties-common \
        wget && \
    rm -rf /var/lib/apt/lists/*

# CMake 3.30 from Kitware's apt repository (jammy ships 3.22).
RUN wget -qO - https://apt.kitware.com/keys/kitware-archive-latest.asc | \
        gpg --dearmor - | \
        tee /usr/share/keyrings/kitware-archive-keyring.gpg >/dev/null && \
    echo "deb [signed-by=/usr/share/keyrings/kitware-archive-keyring.gpg] https://apt.kitware.com/ubuntu/ $(lsb_release -cs) main" \
        > /etc/apt/sources.list.d/kitware.list && \
    apt-get update && \
    apt-get install -y --no-install-recommends cmake=3.30.* cmake-data=3.30.* && \
    rm -rf /var/lib/apt/lists/*

# Zivid SDK (u22 amd64 .debs). apt resolves any missing dependencies.
RUN mkdir -p /tmp/zivid && cd /tmp/zivid && \
    wget -q \
        https://downloads.zivid.com/sdk/releases/${ZIVID_VERSION}/u22/amd64/zivid_${ZIVID_VERSION}_amd64.deb \
        https://downloads.zivid.com/sdk/releases/${ZIVID_VERSION}/u22/amd64/zivid-tools_${ZIVID_VERSION}_amd64.deb \
        https://downloads.zivid.com/sdk/releases/${ZIVID_VERSION}/u22/amd64/zivid-genicam_${ZIVID_VERSION}_amd64.deb && \
    apt-get update && \
    apt-get install -y --no-install-recommends ./zivid_${ZIVID_VERSION}_amd64.deb \
                                                 ./zivid-tools_${ZIVID_VERSION}_amd64.deb \
                                                 ./zivid-genicam_${ZIVID_VERSION}_amd64.deb && \
    rm -rf /tmp/zivid /var/lib/apt/lists/*

# Conan 2.x and a default profile.
RUN pip3 install --no-cache-dir 'conan>=2.0,<3.0' && \
    conan profile detect --force

# Route the compiler through ccache for incremental builds.
ENV PATH="/usr/lib/ccache:${PATH}"

# Pre-populate the conan cache with the viam-cpp-sdk recipe so subsequent
# `conan install` calls can resolve viam-cpp-sdk/<version> without a remote.
RUN git clone --depth 1 --branch releases/v${VIAM_CPP_SDK_VERSION} \
        https://github.com/viamrobotics/viam-cpp-sdk.git /tmp/viam-cpp-sdk && \
    cd /tmp/viam-cpp-sdk && \
    conan export . && \
    rm -rf /tmp/viam-cpp-sdk

WORKDIR /src
