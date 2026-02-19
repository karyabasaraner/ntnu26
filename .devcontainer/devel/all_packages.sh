#!/bin/bash

set -eux
export DEBIAN_FRONTEND=noninteractive

main() {
    local pkgs=(
        clang-format
        clang-tidy-18
        clangd
        cmake
        gdb
        libeigen3-dev
    )

    apt-get update
    apt-get upgrade -y
    apt-get -y --quiet --no-install-recommends install "${pkgs[@]}"

    apt-get -y autoremove
    apt-get clean autoclean
    rm -rf /var/lib/apt/lists/{apt,dpkg,cache,log} /tmp/* /var/tmp/*
}

main "$@"
