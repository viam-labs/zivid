# shellcheck shell=bash
# Resolves which Zivid .deb this host takes off the downloads server. Sourced
# by bin/install-zivid-sdk.sh and bin/install-zivid-tools.sh, which fetch
# different packages from the same release tree and would otherwise carry two
# copies of the distro table free to drift apart. Defines a function and has no
# side effects of its own, so it is sourced rather than executed.
#
# zivid_deb_target <caller> sets, on success:
#
#   UBUNTU_TAG    Ubuntu .deb flavor published by Zivid (u20 / u22 / u24)
#   INSTALL_MODE  "apt" on Ubuntu, "extract" on Debian
#   ARCH          dpkg architecture, part of every .deb filename
#   ARCH_SUBPATH  path segment ahead of the filename ("" or "arm64/")
#
# On an unsupported host it writes a diagnostic prefixed with <caller> and
# returns non-zero rather than exiting, because the two callers disagree on
# what that means: the SDK install cannot proceed without a package, while the
# tools install is a convenience that steps aside.

# The four variables above are this function's return values, read by the
# sourcing script and never by this file.
# shellcheck disable=SC2034
zivid_deb_target() {
    local caller="$1"
    local os
    os="$(uname -s)"
    if [[ "$os" != "Linux" ]]; then
        echo "${caller}: only Linux is supported (got $os)" >&2
        return 1
    fi

    if [[ ! -f /etc/os-release ]]; then
        echo "${caller}: cannot detect distribution (missing /etc/os-release)" >&2
        return 1
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
            echo "${caller}: unsupported distribution ${ID:-unknown}/${VERSION_ID:-unknown}" >&2
            return 1
            ;;
    esac

    ARCH="$(dpkg --print-architecture)"
    case "$ARCH" in
        amd64) ARCH_SUBPATH="" ;;
        # Zivid hosts arm64 .deb's one directory deeper than amd64
        # (e.g. .../u22/arm64/zivid_<rel>_arm64.deb).
        arm64) ARCH_SUBPATH="arm64/" ;;
        *)
            echo "${caller}: unsupported architecture $ARCH" >&2
            return 1
            ;;
    esac
}
