# core

Core is the perception stack for the Field Robotics Lab at MTP, NTNU.

## Development Environment:
The intended development environment is using [Visual Studio Code](https://code.visualstudio.com/) [devcontainers](https://code.visualstudio.com/docs/devcontainers/containers).

The minimum requirements are having docker installed, be careful to follow the complete [installation guide](https://docs.docker.com/engine/install/) and the [Nvidia container toolkit](https://docs.nvidia.com/datacenter/cloud-native/container-toolkit/latest/install-guide.html).

Once these requirements are install, clone this repository, open in VScode and select "Reopen in Container".

The recommended (and supported) system is Ubuntu (24.04) with a Nvidia GPU. Other Ubuntu version or Linux distribution should work with no issues.
If you are on Windows, we wish you all the best luck; consider using devcontainers in WSL, but the challenge probably lies in access to the GPU.
