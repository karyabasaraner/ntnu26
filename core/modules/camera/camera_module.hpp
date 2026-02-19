#ifndef WORKSPACES_CORE_CORE_MODULES_CAMERA_CAMERA_MODULE_HPP
#define WORKSPACES_CORE_CORE_MODULES_CAMERA_CAMERA_MODULE_HPP

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "../utils/configs.hpp"
#include "camera.hpp"

namespace core {

class CameraModule {
/*
    Camera module reads config, initializes cameras.
*/
public:
    explicit CameraModule(const std::string& config_path);

    uint8_t get_num_cameras() const;
    void stop_cameras(size_t index = -1);

private:
    Config _config;
    std::vector<std::unique_ptr<Camera>> _cameras;

    void _initialize_cameras();
};

} // namespace core
#endif
