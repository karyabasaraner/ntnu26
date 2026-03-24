import cv2
import matplotlib.pyplot as plt
import numpy as np
import os
import sys
from collections import deque

sys.path.insert(0, os.path.abspath("build"))
import core

camera_readers = [core.make_reader("configs/four-cameras.yaml", "front_left"),
                  core.make_reader("configs/four-cameras.yaml", "front_right"),
                  core.make_reader("configs/four-cameras.yaml", "left"),
                  core.make_reader("configs/four-cameras.yaml", "right")]
imu_readers = [core.make_reader("configs/four-cameras.yaml", "accelerometer"),
               core.make_reader("configs/four-cameras.yaml", "gyroscope")]

ROLLING_WINDOW = 100
IMU_CHANNELS = ["accelerometer", "gyroscope"]
imu_history = {
    channel: [deque(maxlen=ROLLING_WINDOW) for _ in range(3)]
    for channel in IMU_CHANNELS
}

def read_camera_frames():
    frames = []
    for reader in camera_readers:
        key, head, seq, ts, array = reader.read()
        img_np = array.reshape(720, 1280, 3)
        resized = cv2.resize(img_np, (320, 240))
        frames.append(resized)
    return frames

def read_imu():
    data = []
    for reader in imu_readers:
        key, head, seq, ts, array = reader.read()
        axis_data = []
        for i in range(3): # x, y, z axis
            axis_data.append(np.frombuffer(array[i*4:(i+1)*4], dtype=np.float32)[0].item())

        if axis_data:
            data.append(axis_data)
    return data

def main():
    fig, axs = plt.subplots(2, 3, figsize=(12, 8))
    camera_axes = [axs[0, 0], axs[0, 1], axs[1, 0], axs[1, 1]]
    for ax in camera_axes:
        ax.axis('off')

    placeholders = [ax.imshow(np.zeros((240, 320, 3), dtype=np.uint8)) for ax in camera_axes]

    imu_axes_map = {
        "accelerometer": axs[0, 2],
        "gyroscope": axs[1, 2]
    }
    imu_lines = {}
    colors = ["r", "g", "b"]
    labels = ["x", "y", "z"]
    for channel, ax in imu_axes_map.items():
        ax.set_title(f"{channel.capitalize()}")
        ax.set_xlim(0, ROLLING_WINDOW)
        ax.set_ylim(-16, 16)
        ax.set_ylabel("value")
        ax.grid(True)
        lines = []
        for color, label in zip(colors, labels):
            line, = ax.plot([], [], color=color, label=label)
            lines.append(line)
        ax.legend()
        imu_lines[channel] = lines

    plt.tight_layout()
    plt.ion()
    fig.show()

    try:
        while plt.fignum_exists(fig.number):
            frames = read_camera_frames()
            for im, frame in zip(placeholders, frames):
                im.set_data(frame)

            imu_samples = read_imu()
            for channel, sample in zip(IMU_CHANNELS, imu_samples):
                if len(sample) != 3:
                    continue
                for axis_idx, value in enumerate(sample):
                    imu_history[channel][axis_idx].append(value)

            for channel, lines in imu_lines.items():
                ax_vals = []
                for axis_idx, line in enumerate(lines):
                    data = list(imu_history[channel][axis_idx])
                    ax_vals.extend(data)
                    line.set_data(np.arange(len(data)), data)
                if ax_vals:
                    span = max(0.1, max(ax_vals) - min(ax_vals))
                    margin = span * 0.1
                    ax = imu_axes_map[channel]
                    ax.set_ylim(min(ax_vals) - margin, max(ax_vals) + margin)

            fig.canvas.draw_idle()
            plt.pause(0.05)
    except KeyboardInterrupt:
        pass
    finally:
        plt.ioff()
        plt.close(fig)

if __name__ == "__main__":
    main()
