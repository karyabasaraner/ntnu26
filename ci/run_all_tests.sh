#!/bin/bash
set -e

# Clean the build directory
# rm -rf build

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

exit $err
