## Flashing the jetson

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
sudo cp scripts/max-isp-vi-clks.sh $LDK_ROOTFS_DIR/opt/nvidia/max_clocks/max-isp-vi-clks.sh -f
sudo chmod +x $LDK_ROOTFS_DIR/opt/nvidia/max_clocks/max-isp-vi-clks.sh
sudo install -Dm644 scripts/max-isp-vi-clks.service $LDK_ROOTFS_DIR/etc/systemd/system/max-isp-vi-clks.service
sudo mkdir -p $LDK_ROOTFS_DIR/etc/systemd/system/multi-user.target.wants
sudo ln -sf ../max-isp-vi-clks.service $LDK_ROOTFS_DIR/etc/systemd/system/multi-user.target.wants/max-isp-vi-clks.service
```

The `max-isp-vi-clks.service` unit runs the clock script as root during boot, so the
`nvpmodel`, `jetson_clocks`, and `/sys/kernel/debug/bpmp/debug/clk/*` writes do not
depend on an interactive `sudo` session.

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

21. Apply the device tree patch by placing it into the `/boot/` dir.

### BMI088 wiring
The following connections need to be made:
```
IO-BOARD-V1, Label 40-pin header
GND, GND
SCK, I2C5_CLK
SDI, I2C5_DAT
INT1, GPIO9
INT2, GPIO8
```
The interrupts result due to:
```
BMI --> Shuttle board / IO-BOARD-V1 --> Pin --> Label
INT1 --> INT1 --> PBB0 --> GPIO9
INT3 --> INT2 --> PBB1 --> GPIO8
```

### BMI088 device tree overlay
0. Some information on this can be found in the [Nvidia developer guide](https://docs.nvidia.com/jetson/archives/r36.4.4/DeveloperGuide/SD/Kernel/Bmi088ImuIioDriver.html)
1. Ensure that the IMU is present on bus 7:
```
sudo i2cdetect -y -r 7
```

should return:
```
     0  1  2  3  4  5  6  7  8  9  a  b  c  d  e  f
00:                         -- -- -- -- -- -- -- -- 
10: -- -- -- -- -- -- -- -- -- 19 -- -- -- -- -- -- 
20: -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- 
30: -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- 
40: -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- 
50: -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- 
60: -- -- -- -- -- -- -- -- -- 69 -- -- -- -- -- -- 
70: -- -- -- -- -- -- -- -- 
```

2. On the host, prepare the pre-processed device tree file
```
export KERNEL_SRC="jetson-flash/kernel_sources/Linux_for_Tegra/source/kernel/kernel-jammy-src"
export KERNEL_SRC_INLUDE=$KERNEL_SRC/include
cpp -nostdinc -undef -x assembler-with-cpp -I $KERNEL_SRC_INLUDE -I $KERNEL_SRC_INCLUDE/dt-bindings -I $KERNEL_SRC/arch/arm64/boot/dts bmi088-overlay.dts > bmi088-overlay.pp.dts
dtc -@ -I dts -O dtb -o bmi088-overlay.dtbo hardware/bmi088-overlay.pp.dts
```

3. Copy it to the `/boot/` dir.
4. Now we want to combine the e-con systems and the BMI overlay:
```
sudo fdtoverlay \
  -i /boot/dtb/kernel_tegra234-p3737-0000+p3701-0005-nv.dtb \
  -o /boot/dtb/kernel_tegra234-p3737-0000+p3701-0005-nv-cam-bmi088.dtb \
  /boot/tegra234-p3701-0000-p3737-0000-two-lane-ar0234.dtbo \
  /boot/bmi088-overlay.dtbo
```

which allows us to replace the `/boot/extlinux/extlinux.conf` entry:
```
LABEL JetsonIO
	MENU LABEL Custom Header Config: <CSI Jetson Camera AR0234>
	LINUX /boot/Image
	FDT /boot/dtb/kernel_tegra234-p3737-0000+p3701-0005-nv-cam-bmi088.dtb
	INITRD /boot/initrd
	APPEND ${cbootargs} root=PARTUUID=dc2e6fd7-b19d-404f-acfa-bc2aa4223bc5 rw rootwait rootfstype=ext4 mminit_loglevel=4 console=ttyTCU0,115200 console=ttyAMA0,115200 firmware_class.path=/etc/firmware fbcon=map:0 nospectre_bhb video=efifb:off console=tty0
	OVERLAYS /boot/tegra234-p3701-0000-p3737-0000-two-lane-ar0234.dtbo
```
