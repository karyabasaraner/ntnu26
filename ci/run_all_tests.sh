#!/bin/bash
set -e

# Mock cameras
sudo modprobe vivid n_devs=4 node_types=0x1,0x1,0x1,0x1 num_inputs=1 vid_cap_nr=0,1,2,3

# Mock IMUs
# 1. Load the kernel modules
sudo modprobe iio_dummy
sudo modprobe iio_trig_hrtimer # for triggers

# 2. Mount configfs
sudo mount -t configfs none /sys/kernel/config

# 3. Create 2 virtual IMU devices (accelerometer and gyroscope)
sudo mkdir -p /sys/kernel/config/iio/devices/dummy/accerelometer
sudo mkdir -p /sys/kernel/config/iio/devices/dummy/gyroscope


# Configure and build with testing enabled
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DENABLE_TESTING=ON
cmake --build build

# Initialize error code
err=0

# Find and run all test_ executables in the build directory, excluding config_utilities
while read -r testfile; do
    echo "Running $testfile"
    "$testfile" || err=1
done < <(
    find build -type f -executable -name 'test_*' \
        ! -path '*/config_utilities/*' \
        ! -path '*/config_utilities-*/*'
)

# Remove the mocked cameras
sudo modprobe -r vivid

# Remove the mocked IMUs
sudo rmdir /sys/kernel/config/iio/devices/dummy/accerelometer
sudo rmdir /sys/kernel/config/iio/devices/dummy/gyroscope
sudo modprobe -r iio_dummy
sudo modprobe -r iio_trig_hrtimer
sudo umount /sys/kernel/config

exit $err
