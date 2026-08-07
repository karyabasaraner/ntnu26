#!/bin/bash

PACKAGE_NAME="dataset_collection"
VENV_DIR="venv/$PACKAGE_NAME"

if [ ! -d "$VENV_DIR" ]; then
    python3 -m venv --system-site-packages "$VENV_DIR"
    source "$VENV_DIR/bin/activate"
    pip3 install --upgrade pip
    pip3 install -e python/dataset_collection

else
    source "$VENV_DIR/bin/activate"
fi
