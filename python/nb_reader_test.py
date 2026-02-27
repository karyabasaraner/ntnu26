import matplotlib.pyplot as plt
import numpy as np
import os
import sys
sys.path.insert(0, os.path.abspath("build"))

import core
readers = [core.make_reader("configs/four-cameras.yaml", "front_left"),
           core.make_reader("configs/four-cameras.yaml", "front_right"),
           core.make_reader("configs/four-cameras.yaml", "left"),
           core.make_reader("configs/four-cameras.yaml", "right")]

frames = []
for reader in readers:
    if not reader.is_ready():
        print("Reader not ready")
        exit(1)

    key, head, seq, ts, array = reader.read()
    print(f"Frame key={key}, head={head}, seq={seq}, ts={ts}")
    frames.append(array)


fig, axs = plt.subplots(2, 2, figsize=(8, 8))
for i, ax in enumerate(axs.flat):
    ax.imshow(frames[i].reshape((720, 1280, 3)))
    ax.axis('off')
plt.tight_layout()
plt.savefig("frames.png")
