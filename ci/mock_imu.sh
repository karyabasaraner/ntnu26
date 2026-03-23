#!/bin/bash

# Cleanup function to remove virtual IMU devices
cleanup() {
    # TODO: For some reason removing this fails, but is still required?
    sudo rmdir /sys/kernel/config/iio/devices/dummy/accerelometer
    sudo rmdir /sys/kernel/config/iio/devices/dummy/gyroscope
    sudo modprobe -r iio_dummy
    sudo modprobe -r iio_trig_hrtimer
    sudo umount /sys/kernel/config
    echo "Cleanup complete."
    exit 0
}

# Trap signals to ensure cleanup on exit
trap cleanup SIGINT SIGTERM EXIT

# 1. Load the kernel modules
sudo modprobe iio_dummy
sudo modprobe iio_trig_hrtimer # for triggers

# 2. Mount configfs
sudo mount -t configfs none /sys/kernel/config

# 3. Create 2 virtual IMU devices (accelerometer and gyroscope)
sudo mkdir -p /sys/kernel/config/iio/devices/dummy/accerelometer
sudo mkdir -p /sys/kernel/config/iio/devices/dummy/gyroscope

echo "Mock IMUs created"
echo "Press Ctrl+C to stop all cameras and cleanup..."

while true; do
    sleep 1
done

