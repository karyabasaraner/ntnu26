import os
import sys
sys.path.insert(0, os.path.abspath("build"))

import numpy as np
import torch
import matplotlib.pyplot as plt
import core_sharedmem as cs
r = cs.make_reader("configs/four-cameras.yaml", "front_left")

if not r.is_ready():
    print("reader is not ready")
    exit(1)
print("reader is ready")

plt_im = None
for i in range(10):
    key, arr, seq, ts = r.read()
    print(f"Frame {i}: key={key}, seq={seq}, ts={ts}")

    if ts <= 0:
        print("Invalid timestamp, skipping frame")
        continue

    frame = arr.reshape(720, 1280, 3)

    if plt_im is None:
        plt_im = plt.imshow(frame)
        plt.axis('off')
        plt.tight_layout()

    else:
        plt_im.set_data(frame)

    plt.imsave(f"frame_{i:03d}.png", frame)
