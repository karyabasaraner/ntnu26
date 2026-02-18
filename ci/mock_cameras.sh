#!/bin/bash

# Cleanup function to kill all camera feeds
cleanup() {
    echo ""
    echo "Stopping all camera feeds..."
    if [[ -n "$PID0" ]]; then kill $PID0 2>/dev/null; fi
    if [[ -n "$PID1" ]]; then kill $PID1 2>/dev/null; fi
    if [[ -n "$PID2" ]]; then kill $PID2 2>/dev/null; fi
    if [[ -n "$PID3" ]]; then kill $PID3 2>/dev/null; fi
    wait
    echo "Removing v4l2loopback module..."
    sudo modprobe -r v4l2loopback
    echo "Cleanup complete."
    exit 0
}

# Trap signals to ensure cleanup on exit
trap cleanup SIGINT SIGTERM EXIT

# Create 4 virtual video devices (video0, video1, video2, video3)
sudo modprobe v4l2loopback video_nr=0,1,2,3

# Wait a moment for devices to be created
sleep 1

# Feed test patterns to each camera in the background
echo "Starting camera feeds..."

# Camera 0 - Test pattern with label
ffmpeg -re -f lavfi -i "testsrc=size=1280x720:rate=30,drawtext=text='CAMERA 0':fontsize=60:fontcolor=white:box=1:boxcolor=black@0.5:x=(w-text_w)/2:y=50" -f v4l2 /dev/video0 2>/dev/null &
PID0=$!

# Camera 1 - SMPTE color bars with label
ffmpeg -re -f lavfi -i "testsrc=size=1280x720:rate=30,drawtext=text='CAMERA 1':fontsize=60:fontcolor=white:box=1:boxcolor=black@0.5:x=(w-text_w)/2:y=50" -f v4l2 /dev/video1 2>/dev/null &
PID1=$!

# Camera 2 - RGB test pattern with label
ffmpeg -re -f lavfi -i "testsrc=size=1280x720:rate=30,drawtext=text='CAMERA 2':fontsize=60:fontcolor=white:box=1:boxcolor=black@0.5:x=(w-text_w)/2:y=50" -f v4l2 /dev/video2 2>/dev/null &
PID2=$!

# Camera 3 - Mandelbrot fractal with label
ffmpeg -re -f lavfi -i "testsrc=size=1280x720:rate=30,drawtext=text='CAMERA 3':fontsize=60:fontcolor=white:box=1:boxcolor=black@0.5:x=(w-text_w)/2:y=50" -f v4l2 /dev/video3 2>/dev/null &
PID3=$!

echo "Mock cameras created:"
echo "  /dev/video0 (testsrc + CAMERA 0 label) - PID: $PID0"
echo "  /dev/video1 (testsrc + CAMERA 1 label) - PID: $PID1"
echo "  /dev/video2 (testsrc + CAMERA 2 label) - PID: $PID2"
echo "  /dev/video3 (testsrc + CAMERA 3 label) - PID: $PID3"
echo ""
echo "Press Ctrl+C to stop all cameras and cleanup..."

# Keep script running until interrupted
wait
