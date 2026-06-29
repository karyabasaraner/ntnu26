#include "camera_module.hpp"
#include "camera_factory.hpp"
#include "configs.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <spdlog/spdlog.h>
#include <string>

namespace core {

CameraModule::CameraModule(const std::string& config_path) {
    _config.load(config_path);
    _initialize_cameras();
}

uint8_t CameraModule::get_num_cameras() const {
    return static_cast<uint8_t>(_cameras.size());
}

uint8_t CameraModule::get_running_cameras() const {
    uint8_t count = 0;
    for (const auto& camera : _cameras) {
        if (camera && camera->is_valid() && camera->is_running()) {
            count++;
        }
    }
    return count;
}

void CameraModule::start_cameras(size_t index) {
    if (index < _cameras.size()) {
        auto& camera = _cameras[index];
        if (camera && camera->is_valid()) {
            camera->start();
        }
    } else {
        for (const auto& camera : _cameras) {
            if (camera && camera->is_valid()) {
                camera->start();
            }
        }
    }
}

void CameraModule::stop_cameras(size_t index) {
    if (index < _cameras.size()) {
        auto& camera = _cameras[index];
        if (camera && camera->is_valid()) {
            camera->stop();
        }
    } else {
        for (const auto& camera : _cameras) {
            if (camera && camera->is_valid()) {
                camera->stop();
            }
        }
    }
}

void CameraModule::_initialize_cameras() {
    for (const CameraConfig& cam_config : _config.get_config().cameras) {
        const char* backend = cam_config.backend == CameraBackend::pylon ? "pylon" : "v4l2";
        spdlog::info("Initializing {} camera: {}", backend, cam_config.name);
        _cameras.push_back(create_camera(cam_config));
    }
}

} // namespace core
