FROM nvidia/cuda:12.9.0-cudnn-devel-ubuntu24.04

ARG TZ=Europe/Zurich
ARG USER_UID=1000
ARG USERNAME=ubuntu
ENV DEBIAN_FRONTEND=noninteractive

# Minimum and all other apt packages
COPY min_packages.sh /tmp/min_packages.sh
RUN /tmp/min_packages.sh && rm -f /tmp/min_packages.sh

COPY all_packages.sh /tmp/all_packages.sh
RUN /tmp/all_packages.sh && rm -f /tmp/all_packages.sh

ENV LANG="en_US.UTF-8" \
    LANGUAGE="en_US:en" \
    LC_ALL="en_US.UTF-8"

# Add user with sudo rights
RUN usermod -u ${USER_UID} -aG dialout,plugdev,sudo ${USERNAME} && \
    mkdir -p /etc/sudoers.d && \
    echo "${USERNAME} ALL=(ALL) NOPASSWD:ALL" > /etc/sudoers.d/${USERNAME}

# Git-prompt: Source
RUN echo 'if [ -f "$HOME/.bash-git-prompt/gitprompt.sh" ]; then GIT_PROMPT_ONLY_IN_REPO=1; source "$HOME/.bash-git-prompt/gitprompt.sh"; fi' >> /home/${USERNAME}/.bash_aliases

# Clean up
RUN rm -rf /tmp/* /var/lib/apt/lists/* /var/tmp/* /var/cache/apt/archives/*

ENTRYPOINT ["/bin/bash"]
