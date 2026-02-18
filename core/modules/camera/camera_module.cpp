#include "camera_module.hpp"
#include "camera.hpp"
#include "configs.hpp"

#include <cstdint>
#include <memory>
#include <string>

namespace core {

CameraModule::CameraModule(const std::string& config_path) {
    // TODO(MJ): Error handling for config?
    _config.load(config_path);

    _initialize_cameras();
}

uint8_t CameraModule::get_num_cameras() const {
    return static_cast<uint8_t>(_cameras.size());
}

void CameraModule::_initialize_cameras() {
    for (const CameraConfig& cam_config : _config.get_config().cameras) {
        Camera const camera(cam_config);
        if (camera.is_valid()) {
            _cameras.push_back(std::make_unique<Camera>(cam_config));
        }
    }
}

} // namespace core
