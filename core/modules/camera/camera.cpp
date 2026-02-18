#include "camera.hpp"

#include "../utils/configs.hpp"

#include <cstdint>
#include <fcntl.h>
#include <filesystem>
#include <iostream>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <utility>

namespace core {

Camera::Camera(CameraConfig config) : _config(std::move(config)) {
}

bool Camera::is_valid() const {
    if (!std::filesystem::exists(_config.device)) {
        std::cout << "Camera device does not exist: " << _config.device << '\n';
        return false;
    }

    const int file_desc = open(_config.device.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC, 0);
    if (file_desc < 0) {
        std::cout << "Failed to open camera device: " << _config.device << '\n';
        return false;
    }

    struct v4l2_capability cap{};

    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg,hicpp-vararg)
    const bool valid = ioctl(file_desc, VIDIOC_QUERYCAP, &cap) >= 0;
    if (!valid) {
        std::cout << "Device is not a valid V4L2 camera: " << _config.device << '\n';;
    }
    close(file_desc);
    std::cout << "Camera " << _config.name << " (" << _config.device << ") is valid.\n";
    return valid;
}

} // namespace core
