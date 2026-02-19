#!/bin/bash
files=$(find . \( -path ./build -o -path ./hardware \) -prune -o -regex '.*\.\(cpp\|hpp\|cc\|c\|h\)' -print)

if [ "$1" == "--check" ]; then
    clang-format --dry-run --Werror $files
else
    clang-format -i $files
fi

if [ $? -ne 0 ]; then
    echo "Some files are not formatted according to .clang-format"
    exit 1
else
    echo "All files are properly formatted."
    exit 0
fi
