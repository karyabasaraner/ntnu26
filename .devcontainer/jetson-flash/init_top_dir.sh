#!/bin/bash

set -euo pipefail

TOP_DIR="${TOP_DIR:-/workspaces/core/.jetson_flash}"

mkdir -p "${TOP_DIR}" "${TOP_DIR}/kernel_sources" "${TOP_DIR}/tool_chain"
