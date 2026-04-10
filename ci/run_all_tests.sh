#!/bin/bash
set -e

_mounted_configfs=0

cleanup() {
    sudo rmdir /sys/kernel/config/iio/devices/dummy/accerelometer 2>/dev/null || true
    sudo rmdir /sys/kernel/config/iio/devices/dummy/gyroscope 2>/dev/null || true
    sudo modprobe -r vivid 2>/dev/null || true
    sudo modprobe -r iio_dummy 2>/dev/null || true
    sudo modprobe -r iio_trig_hrtimer 2>/dev/null || true

    if [[ "${_mounted_configfs}" -eq 1 ]]; then
        sudo umount /sys/kernel/config 2>/dev/null || true
    fi
}

trap cleanup EXIT

# Mock cameras
sudo modprobe vivid n_devs=4 node_types=0x1,0x1,0x1,0x1 num_inputs=1 vid_cap_nr=0,1,2,3

# Mock IMUs
# 1. Load the kernel modules
sudo modprobe iio_dummy
sudo modprobe iio_trig_hrtimer # for triggers

# 2. Mount configfs
if ! mountpoint -q /sys/kernel/config; then
    sudo mount -t configfs none /sys/kernel/config
    _mounted_configfs=1
fi

# 3. Create 2 virtual IMU devices (accelerometer and gyroscope)
sudo mkdir -p /sys/kernel/config/iio/devices/dummy/accerelometer
sudo mkdir -p /sys/kernel/config/iio/devices/dummy/gyroscope


# Configure and build with testing enabled
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DENABLE_TESTING=ON
cmake --build build

# Initialize error code
err=0

# Collect the tests we intend to execute; optionally run a single requested test
test_request="$1"
common_filters=(
    ! -path '*/config_utilities/*'
    ! -path '*/config_utilities-*/*'
)
tests=()
if [[ -n "${test_request}" ]]; then
    if [[ "${test_request}" == */* ]]; then
        tests=("${test_request}")
    else
        mapfile -t tests < <(
            find build -type f -executable -name "${test_request}" "${common_filters[@]}"
        )
    fi
else
    mapfile -t tests < <(
        find build -type f -executable -name 'test_*' "${common_filters[@]}"
    )
fi

# Run the selected tests
tests_run=0
for testfile in "${tests[@]}"; do
    tests_run=$((tests_run + 1))
    echo "Running $testfile"
    "$testfile" || err=1
done
if [[ $tests_run -eq 0 ]]; then
    echo "No matching tests found." >&2
    err=1
fi

exit $err
