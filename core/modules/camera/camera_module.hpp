#ifndef CAMERA_MODULE_HPP
#define CAMERA_MODULE_HPP

#include <string>

#include "../utils/configs.hpp"

namespace core {

class CameraModule {
/*
    Camera module reads config, initializes cameras.
*/
public:
    explicit CameraModule(std::string config_path);

private:
    Config _config;
};

} // namespace core
#endif