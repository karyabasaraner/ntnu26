#include "camera_module.hpp"

namespace core {

CameraModule::CameraModule(std::string config_path) {
    _config.load(config_path);
}

} // namespace core
