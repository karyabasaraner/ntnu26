#!/bin/bash

: "${TOP_DIR:=/workspaces/core/jetson-flash}"
: "${L4T_VERSION:=36.4.0}"
: "${RELEASE_PACK_NAME:=e-CAM20_CUOAGX_JETSON_AGX_ORIN_L4T36.4.0_7-NOV-2024_R01}"

if [ -z "${RELEASE_PACK_NAME}" ]; then
    RELEASE_PACK_NAME="eCAM20_CUOAGX_JETSON_AGX_ORIN_${L4T_VERSION}_<release_date>_<release_version>"
fi

export TOP_DIR
export RELEASE_PACK_NAME
export RELEASE_PACK_DIR="${TOP_DIR}/${RELEASE_PACK_NAME}"
export L4T_DIR="${TOP_DIR}/Linux_for_Tegra"
export LDK_ROOTFS_DIR="${L4T_DIR}/rootfs"
export ARCH=arm64
export CROSS_COMPILE="${TOP_DIR}/tool_chain/aarch64--glibc--stable-2022.08-1/bin/aarch64-buildroot-linux-gnu-"
export NVIDIA_SRC="${TOP_DIR}/kernel_sources/Linux_for_Tegra/source"
export KERNEL_HEADERS="${NVIDIA_SRC}/kernel/kernel-jammy-src"
export KERNEL_OUTPUT=$KERNEL_HEADERS
export INSTALL_MOD_PATH="${LDK_ROOTFS_DIR}"
export TEGRA_KERNEL_OUT="${NVIDIA_SRC}/out/nvidia-linux-header"
export SENSOR_DRIVER="e-CAM20_CUOAGX"
