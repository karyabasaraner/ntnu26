#!/bin/bash
set -e

# Clean the build directory
# rm -rf build

# Configure and build with testing enabled
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DENABLE_TESTING=ON
cmake --build build

# Path to clang-tidy-diff.py (adjust if needed)
CLANG_TIDY_DIFF=ci/clang-tidy-diff.py
COMPILE_COMMANDS=build/compile_commands.json
DIFF_BASE=origin/main

# Generate diff
git diff -U0 $DIFF_BASE | \
  python3 $CLANG_TIDY_DIFF \
    -p1 \
    -clang-tidy-binary clang-tidy-18 \
    -path $COMPILE_COMMANDS \
    -export-fixes=clang-tidy-fixes.yaml \
    -fix \
    -j $(nproc)

echo "clang-tidy diff completed."
