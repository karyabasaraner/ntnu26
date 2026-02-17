#!/bin/bash

set -eux
export DEBIAN_FRONTEND=noninteractive

main() {
    local pkgs=(
        bc
        bison
        build-essential
        cpio
        dosfstools
        flex
        kmod
        lbzip2
        libssl-dev
        libxml2-utils
        openssl
        python-is-python3
        python3
        python3-yaml
        qemu-user-static
        uuid-runtime
        xz-utils
    )

    apt-get update
    apt-get upgrade -y
    apt-get -y --quiet --no-install-recommends install "${pkgs[@]}"

    apt-get -y autoremove
    apt-get clean autoclean
    rm -rf /var/lib/apt/lists/{apt,dpkg,cache,log} /tmp/* /var/tmp/*
}

main "$@"
