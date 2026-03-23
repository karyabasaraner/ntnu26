#!/bin/bash

set -eux
export DEBIAN_FRONTEND=noninteractive

main() {
    local pkgs=(
        bison
        ccache
        clang-format
        clang-tidy-18
        clangd
        cmake
        ffmpeg
        flex
        gdb
        gstreamer1.0-plugins-base
        gstreamer1.0-plugins-good
        gstreamer1.0-tools
        jq
        kmod
        libaio-dev
        libboost-dev
        libboost-filesystem-dev
        libboost-system-dev
        libeigen3-dev
        libgstreamer-plugins-base1.0-dev
        libgtest-dev
        libiio-utils
        libopencv-dev
        libspdlog-dev
        libusb-1.0-0-dev
        libxml2-dev
        libyaml-cpp-dev
        python3-dev
        python3.12-venv
        rsync
        v4l-utils
        zlib1g-dev
    )

    apt-get update
    apt-get upgrade -y
    apt-get -y --quiet --no-install-recommends install "${pkgs[@]}"

    apt-get -y autoremove
    apt-get clean autoclean
    rm -rf /var/lib/apt/lists/{apt,dpkg,cache,log} /tmp/* /var/tmp/*
}

main "$@"
