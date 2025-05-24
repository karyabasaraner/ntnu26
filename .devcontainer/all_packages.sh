#!/bin/bash

set -eux
export DEBIAN_FRONTEND=noninteractive

main() {
    local pkgs=(
        clangd
        cmake
    )

    apt-get update
    apt-get upgrade -y
    apt-get -y --quiet --no-install-recommends install "${pkgs[@]}"

    apt-get -y autoremove
    apt-get clean autoclean
    rm -rf /var/lib/apt/lists/{apt,dpkg,cache,log} /tmp/* /var/tmp/*
}

main "$@"
