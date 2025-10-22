#!/bin/bash

PACKAGE_NAME="calibration"
VENV_DIR="venv/$PACKAGE_NAME"

if [ ! -d "$VENV_DIR" ]; then
    python3 -m venv "$VENV_DIR"
    source "$VENV_DIR/bin/activate"
    pip3 install --upgrade pip
    pip3 install -e python/calibration

else
    source "$VENV_DIR/bin/activate"
fi
