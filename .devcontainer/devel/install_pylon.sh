#!/usr/bin/env bash

set -euo pipefail
export DEBIAN_FRONTEND=noninteractive

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
shopt -s nullglob
archives=("${script_dir}"/pylon-*_linux-x86_64_debs.tar.gz)

if (( ${#archives[@]} == 0 )); then
    echo "Optional Pylon SDK archive not found in ${script_dir}; skipping installation."
    exit 0
fi

if (( ${#archives[@]} > 1 )); then
    echo "Multiple Pylon SDK archives found in ${script_dir}; keep exactly one." >&2
    exit 1
fi

tmpdir="$(mktemp -d)"
trap 'rm -rf "${tmpdir}"' EXIT

echo "Extracting Pylon SDK from ${archives[0]}"
tar -xzf "${archives[0]}" -C "${tmpdir}"

debs=("${tmpdir}"/*.deb)
if (( ${#debs[@]} == 0 )); then
    echo "Pylon SDK archive contains no Debian packages." >&2
    exit 1
fi

apt-get update
apt-get install -y --no-install-recommends "${debs[@]}"
rm -rf /var/lib/apt/lists/* /var/cache/apt/archives/*

test -f "${PYLON_ROOT:-/opt/pylon}/share/pylon/cmake/pylon-config.cmake"
echo "Pylon SDK installed successfully."
