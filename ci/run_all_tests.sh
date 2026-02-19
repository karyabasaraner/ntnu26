#!/bin/bash
set -e

# Clean the build directory
rm -rf build

# Configure and build with testing enabled
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DENABLE_TESTING=ON
cmake --build build

# Initialize error code
err=0

# Find and run all test_ executables in the build directory
find build -type f -executable -name 'test_*' | while read -r testfile; do
    echo "Running $testfile"
    "$testfile" || err=1
done

exit $err
