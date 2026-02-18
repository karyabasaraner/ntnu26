#!/bin/bash

# Cleanup function to kill all camera feeds
cleanup() {
    sudo modprobe -r vivid
    echo "Cleanup complete."
    exit 0
}

# Trap signals to ensure cleanup on exit
trap cleanup SIGINT SIGTERM EXIT

# Create 4 virtual video devices (video0, video1, video2, video3)
# https://www.kernel.org/doc/html/v4.8/media/v4l-drivers/vivid.html
sudo modprobe vivid n_devs=4 node_types=0x1,0x1,0x1,0x1 num_inputs=1 vid_cap_nr=0,1,2,3

echo "Mock cameras created"
echo "Press Ctrl+C to stop all cameras and cleanup..."

while true; do
    sleep 1
done

