import argparse
import os
import sys

import matplotlib.pyplot as plt
import numpy as np

sys.path.insert(0, os.path.abspath("build"))
import core


def plot_imu(log_file):
    imu_topics = log_file.imu_topics()
    if not imu_topics:
        return

    fig, axes = plt.subplots(len(imu_topics), 1, squeeze=False, figsize=(10, 3 * len(imu_topics)))
    for row, topic in enumerate(imu_topics):
        series = log_file.get_imu_data(topic)
        if series is None:
            continue

        time_s = (series["timestamp_ns"] - series["timestamp_ns"][0]) / 1e9
        ax = axes[row][0]
        ax.plot(time_s, series["x"], label="x")
        ax.plot(time_s, series["y"], label="y")
        ax.plot(time_s, series["z"], label="z")
        ax.set_title(topic)
        ax.set_xlabel("time [s]")
        ax.set_ylabel("value")
        ax.grid(True)
        ax.legend()


def show_first_camera_frame(log_file):
    camera_topics = log_file.camera_topics()
    if not camera_topics:
        return

    try:
        import cv2
    except ImportError:
        print("OpenCV is not installed; skipping camera preview")
        return

    topic = camera_topics[0]
    series = log_file.get_camera_data(topic)
    if series is None or not series["jpeg_data"]:
        return

    encoded = np.frombuffer(series["jpeg_data"][0], dtype=np.uint8)
    image = cv2.imdecode(encoded, cv2.IMREAD_COLOR)
    if image is None:
        print(f"Could not decode first camera frame for {topic}")
        return

    image = cv2.cvtColor(image, cv2.COLOR_BGR2RGB)
    plt.figure(figsize=(8, 5))
    plt.imshow(image)
    plt.axis("off")
    plt.title(topic)


def main():
    parser = argparse.ArgumentParser(description="Read and plot a core MCAP log file")
    parser.add_argument("mcap_path", help="Path to the MCAP file")
    args = parser.parse_args()

    log_file = core.read_log_file(args.mcap_path)
    print(log_file.format_metadata())
    plot_imu(log_file)
    show_first_camera_frame(log_file)
    plt.show()


if __name__ == "__main__":
    main()
