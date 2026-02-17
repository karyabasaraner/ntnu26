#!/usr/bin/env bash
set -euo pipefail

FPS=30
WIDTH=1280
HEIGHT=720

usage() {
  echo "Usage: $0 [video_index|/dev/videoX]"
  echo "  No args: show 4 cams in a 2x2 grid (/dev/video0-3)."
  echo "  With arg: show a single cam. Examples: $0 2  OR  $0 /dev/video2"
}

if [[ "${1-}" == "-h" || "${1-}" == "--help" ]]; then
  usage
  exit 0
fi

caps="video/x-raw,width=$WIDTH,height=$HEIGHT,framerate=$FPS/1"

if [[ -z "${1-}" ]]; then
  exec gst-launch-1.0 \
    compositor name=comp \
      sink_0::xpos=0    sink_0::ypos=0 \
      sink_1::xpos=$WIDTH sink_1::ypos=0 \
      sink_2::xpos=0    sink_2::ypos=$HEIGHT \
      sink_3::xpos=$WIDTH sink_3::ypos=$HEIGHT \
    ! videoconvert ! autovideosink \
    v4l2src device=/dev/video0 ! videoconvert ! "$caps" ! comp.sink_0 \
    v4l2src device=/dev/video1 ! videoconvert ! "$caps" ! comp.sink_1 \
    v4l2src device=/dev/video2 ! videoconvert ! "$caps" ! comp.sink_2 \
    v4l2src device=/dev/video3 ! videoconvert ! "$caps" ! comp.sink_3
fi

if [[ "${1}" =~ ^/dev/video[0-9]+$ ]]; then
  dev="$1"
elif [[ "${1}" =~ ^[0-9]+$ ]]; then
  dev="/dev/video${1}"
else
  echo "Invalid device: $1" >&2
  usage
  exit 2
fi

if [[ ! -e "$dev" ]]; then
  echo "Device not found: $dev" >&2
  exit 3
fi

exec gst-launch-1.0 v4l2src device="$dev" ! videoconvert ! "$caps" ! videoconvert ! autovideosink
