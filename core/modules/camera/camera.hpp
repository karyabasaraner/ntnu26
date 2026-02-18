#ifndef WORKSPACES_CORE_CORE_MODULES_CAMERA_CAMERA_HPP
#define WORKSPACES_CORE_CORE_MODULES_CAMERA_CAMERA_HPP

#include "../utils/configs.hpp"

namespace core {

class Camera {
public:
    explicit Camera(CameraConfig config);

    bool is_valid() const;

private:
    CameraConfig _config;
};

} // namespace core

#endif