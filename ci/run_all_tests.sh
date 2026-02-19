#!/bin/bash
set -e

# Clean the build directory
# rm -rf build

# Mock the cameras required for all testing
sudo modprobe vivid n_devs=4 node_types=0x1,0x1,0x1,0x1 num_inputs=1 vid_cap_nr=0,1,2,3

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

exit $err
