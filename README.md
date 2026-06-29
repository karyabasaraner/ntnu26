# core
core is a vision perception stack used at the NTNU FRL.

## Building
Release build:
```
cmake -B build -DCMAKE_BUILD_TYPE=Release
```
or debug build:
```
cmake -B build -DCMAKE_BUILD_TYPE=Debug
```
and then build with:
```
cmake --build build -j
```

### Optional Basler Pylon camera support

Basler GigE cameras are disabled by default, so a normal build does not require the Pylon SDK.
Install the Basler Pylon Software Suite development package and enable the backend explicitly:

```
export PYLON_ROOT=/opt/pylon
cmake -B build -DCMAKE_BUILD_TYPE=Release -DENABLE_PYLON=ON
cmake --build build -j
```

The SDK must provide its `pylon-config.cmake` package and `pylon::pylon` target.
If Pylon is installed outside `/opt/pylon`, set `PYLON_ROOT` or add its install prefix to `CMAKE_PREFIX_PATH`.

The development container installs Pylon automatically during image creation when exactly one `pylon-*_linux-x86_64_debs.tar.gz` archive is present in `.devcontainer/devel`.
The archive is optional and is not copied into an image layer.

`configs/basler-dart.yaml` is a minimal configuration for the color Basler dart M `dmA720-290gc` in a four-camera layout.
Each camera selects its backend by containing exactly one `v4l2` or `pylon` mapping.
Update the Pylon IP addresses and configure the host NIC on the same subnet.
This camera module requires a dart M GigE interface board or GigE switch.
The backend acquires `BayerRG8`, converts it to packed RGB8 with Pylon, and publishes it through the existing shared-memory path used by the web frontend and MCAP logger.

### Requirements Jetson
```
sudo apt install ccache \
                 clang-format \
                 clangd \
                 cmake \
                 cuda-toolkit \
                 ffmpeg \
                 gdb \
                 gstreamer1.0-plugins-base \
                 gstreamer1.0-plugins-good \
                 gstreamer1.0-tools \
                 jq \
                 kmod \
                 libboost-dev \
                 libboost-filesystem-dev \
                 libboost-system-dev \
                 libeigen3-dev \
                 libgstreamer-plugins-base1.0-dev \
                 libgtest-dev \
                 libopencv-dev \
                 libspdlog-dev \
                 nvidia-opencv \
                 nvidia-opencv-dev \
                 python3-dev \
                 python3.10-venv \
                 v4l-utils \
                 zlib1g-dev
```

## Coordinate convention
The stack utilizes passive transformations all throughout the code.
A passive transformation represents a change in frame of reference of e.g. a point, compared to an active transformation, that represent a change of the pose of the point itself.
The nomenclature is $\Phi_{\mathcal{A}\mathcal{B}}$ or in code `phi_A_B`, which is a passive transformation from frame $\mathcal{B}$ to frame $\mathcal{A}$.
It is worth to note that different notations exists (e.g. see [1]), but the advantage of this formulation is that it is very intuitive,
$$\Phi_{\mathcal{A}\mathcal{C}} = \Phi_{\mathcal{A}\mathcal{B}}\Phi_{\mathcal{B}\mathcal{C}}$$
since the $\mathcal{B}$ cancels out.
Therefore we stick to this formulation.
![coordinate_conventions](docs/coordinate_conventions.svg)
Quaternions or rotation matrices are preferred; use of Euler angles should be motivated carefully and are rarely the best choice.

### On the representation of poses and transformations
As a concrete example and reminder of this convention, let's consider two poses $\mathcal{A}$, $\mathcal{B}$ in inertial frame $\mathcal{I}$, ${}\_\mathcal{I}\Phi_{\mathcal{A}}$, ${}\_\mathcal{I}\Phi_{\mathcal{B}}$.
This is equivalent to writing it as transformations $\Phi_{\mathcal{I}\mathcal{A}}$, $\Phi_{\mathcal{I}\mathcal{B}}$, since in order to obtain the transformation from frame $\mathcal{B}$ to frame $\mathcal{A}$, we can use the following relation:
$$\Phi_{\mathcal{A}\mathcal{B}} = \Phi_{\mathcal{I}\mathcal{A}}^{-1}\Phi_{\mathcal{I}\mathcal{B}}$$

### Quaternions
We use the Hamilton [2] convention and denote $q_0\in\mathbb{R}$ as the real part, and $\mathbf{q}\in\mathbb{R}^3$. Following [1], we denote the transformation then as $\Phi = \left(q_0,\mathbf{q}\right)$.

### References
[1] Bloesch, M., Sommer, H., Laidlow, T., Burri, M., Nuetzi, G., Fankhauser, P., Bellicoso, D., Gehring, C., Leutenegger, S., Hutter, M., & Siegwart, R. (2016). A Primer on the Differential Calculus of 3D Orientations. https://arxiv.org/abs/1606.05285
[2] W. R. Hamilton, “On quaternions; or on a new system of imaginaries in algebra,” The London, Edinburgh, and Dublin Philosophical Magazine and Journal of Science, vol. 25, no. 163, pp. 10–13, 1844
