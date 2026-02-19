#!/bin/bash

set -eux
export DEBIAN_FRONTEND=noninteractive

main() {
    local pkgs=(
        ccache
        clang-format
        clang-tidy-18
        clangd
        cmake
        ffmpeg
        gdb
        gstreamer1.0-plugins-base
        gstreamer1.0-plugins-good
        gstreamer1.0-tools
        jq
        kmod
        libboost-dev
        libboost-filesystem-dev
        libboost-system-dev
        libeigen3-dev
        libgstreamer-plugins-base1.0-dev
        libgtest-dev
        libspdlog-dev
        v4l-utils
    )

    apt-get update
    apt-get upgrade -y
    apt-get -y --quiet --no-install-recommends install "${pkgs[@]}"

    apt-get -y autoremove
    apt-get clean autoclean
    rm -rf /var/lib/apt/lists/{apt,dpkg,cache,log} /tmp/* /var/tmp/*
}

main "$@"
