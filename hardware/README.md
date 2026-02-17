## flashing jetson

1. Move the e-con Systems release package tar file to the staging directory:
```
mv <location of>/e-
CAM20_CUOAGX_JETSON_AGX_ORIN_<L4T_version>_<r
elease_date>_<release_version>.tar.gz jetson_flash
```

2. Download the Bootlin toolchain from [developer.nvidia.com](https://developer.nvidia.com/downloads/embedded/l4t/r36_release_v3.0/toolchain/aarch64--glibc--stable-2022.08-1.tar.bz2) and move and extract it to the `tool_chain` folder.
```
mv ~/Downloads/aarch64--glibc--stable-2022.08-1.tar.bz2 $TOP_DIR/tool_chain
cd $TOP_DIR/tool_chain
tar -xf aarch64--glibc--stable-2022.08-1.tar.bz2
```

3. Download the L4T Jetson driver package and sample rootfs from [developer.nvidia.com](https://developer.nvidia.com/downloads/embedded/l4t/r36_release_v4.0/release/Jetson_Linux_R36.4.0_aarch64.tbz2) and [developer.nvidia.com](https://developer.nvidia.com/downloads/embedded/l4t/r36_release_v4.0/release/Tegra_Linux_Sample-Root-Filesystem_R36.4.0_aarch64.tbz2) to the `$TOP_DIR`.
```
cp $HOME/Downloads/Jetson_Linux_R36.4.0_aarch64.tbz2 $TOP_DIR
cp $HOME/Downloads/Tegra_Linux_Sample-Root-Filesystem_R36.4.0_aarch64.tbz2 $TOP_DIR
```

4. Extract and prepare L4T
```
cd $TOP_DIR
tar xf Jetson_Linux_R36.4.0_aarch64.tbz2
```

5. Extract sample file system to rootfs
```
sudo tar xpf Tegra_Linux_Sample-Root-Filesystem_R36.4.0_aarch64.tbz2 -C $LDK_ROOTFS_DIR
```

5. Make sure that the host has the right qemu packages:
```
sudo apt-get update
sudo apt-get install -y qemu-user-static binfmt-support
sudo update-binfmts --enable qemu-aarch64
sudo systemctl restart systemd-binfmt
```

6. Set the package to be ready to flash binaries
```
cd $L4T_DIR
sudo ./tools/l4t_flash_prerequisites.sh
sudo ./apply_binaries.sh
```

7. Extract the release package
```
cd $TOP_DIR
tar -xaf e-CAM20_CUOAGX_JETSON_AGX_ORIN_L4T36.4.0_7-NOV-2024_R01.tar.gz
```

8. Downloading the kernel sources from [developer.nvidia.com](https://developer.nvidia.com/downloads/embedded/l4t/r36_release_v4.0/sources/public_sources.tbz2) and copy them to kernel sources.
```
cp $HOME/Downloads/public_sources.tbz2 $TOP_DIR/kernel_sources
```

9. Extract the kernel source
```
cd $TOP_DIR/kernel_sources
tar xf public_sources.tbz2
cd $NVIDIA_SRC
tar xf kernel_src.tbz2
tar xf kernel_oot_modules_src.tbz2
tar xf nvidia_kernel_display_driver_source.tbz2
```

10. Apply the nvidia-oot patch
```
patch -p1 -i $RELEASE_PACK_DIR/Kernel/e-CAM20_CUOAGX_JETSON_ORIN-AGX_L4T36.4.0_oot.patch --dry-run
patch -p1 -i $RELEASE_PACK_DIR/Kernel/e-CAM20_CUOAGX_JETSON_ORIN-AGX_L4T36.4.0_oot.patch
```
TODO this is not working

11. Apply the device tree patch
```
patch -p1 -i $RELEASE_PACK_DIR/Kernel/e-CAM20_CUOAGX_JETSON_ORIN-AGX_L4T36.4.0_dtb.patch --dry-run
patch -p1 -i $RELEASE_PACK_DIR/Kernel/e-CAM20_CUOAGX_JETSON_ORIN-AGX_L4T36.4.0_dtb.patch
```

12. Apply the sensor-driver module patch
```
mkdir -p sensor_driver
patch -d sensor_driver -p1 -i $RELEASE_PACK_DIR/Kernel/e-CAM20_CUOAGX_JETSON_ORIN-AGX_L4T36.4.0_module.patch --dry-run
patch -d sensor_driver -p1 -i $RELEASE_PACK_DIR/Kernel/e-CAM20_CUOAGX_JETSON_ORIN-AGX_L4T36.4.0_module.patch
```

13. Build and install the kernel and modules
```
make -C kernel
make modules
sudo -E make modules_install
make dtbs
```

14. Copy kernel and dtp files to L4T_DIR
```
sudo cp kernel/kernel-jammy-src/arch/arm64/boot/Image $LDK_ROOTFS_DIR/boot/ -f
sudo cp kernel-devicetree/generic-dts/dtbs/tegra234-p3701-0000-p3737-0000-two-lane-ar0234.dtbo $LDK_ROOTFS_DIR/boot/ -f
```

15. Modifying the rootfs
```
sudo mkdir -p $LDK_ROOTFS_DIR/opt/nvidia/max_clocks
sudo cp $RELEASE_PACK_DIR/misc/max-isp-vi-clks.sh $LDK_ROOTFS_DIR/opt/nvidia/max_clocks/max-isp-vi-clks.sh -f
sudo chmod +x $LDK_ROOTFS_DIR/opt/nvidia/max_clocks/max-isp-vi-clks.sh
```

16. Update v4l2 compliance TODO Not working
```
sudo cp $RELEASE_PACK_DIR/misc/v4l2-compliance $LDK_ROOTFS_DIR/usr/local/bin/ -f
```

17. Put Jetson into recovery mode: Hold recovery (middle) and power (opposite side of SD card slot) button while powering.
`lsusb` should show `Bus 00X Device 00Y: ID 0955:7023 NVidia Corp.`
and `ls -l /dev/bus/usb/00X/00Y` should be present.

18. Move to the host, to flash the Jetson.
```
cd $L4T_DIR
sudo ./flash.sh jetson-agx-orin-devkit internal
```

19. Configure Jetson in desktop GUI

20. Load the drivers:
```
sudo /opt/nvidia/jetson-io/config-by-hardware.py -n 2="Jetson Camera AR0234"
```
and reboot.
