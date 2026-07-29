#include "event_camera_module.hpp"
#include "configs.hpp"
#include "event_camera_factory.hpp"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <spdlog/spdlog.h>
#include <string>

namespace core {

EventCameraModule::EventCameraModule(const std::string& config_path) {
    _config.load(config_path);
    _initialize_event_cameras();
}

uint8_t EventCameraModule::get_num_event_cameras() const {
    return static_cast<uint8_t>(_event_cameras.size());
}

uint8_t EventCameraModule::get_running_event_cameras() const {
    uint8_t count = 0;
    for (const auto& event_camera : _event_cameras) {
        if (event_camera && event_camera->is_valid() && event_camera->is_running()) {
            count++;
        }
    }
    return count;
}

void EventCameraModule::start_event_cameras(size_t index) {
    if (index < _event_cameras.size()) {
        auto& event_camera = _event_cameras[index];
        if (event_camera && event_camera->is_valid()) {
            event_camera->start();
        }
    } else {
        for (const auto& event_camera : _event_cameras) {
            if (event_camera && event_camera->is_valid()) {
                event_camera->start();
            }
        }
    }
}

void EventCameraModule::stop_event_cameras(size_t index) {
    if (index < _event_cameras.size()) {
        auto& event_camera = _event_cameras[index];
        if (event_camera && event_camera->is_valid()) {
            event_camera->stop();
        }
    } else {
        for (const auto& event_camera : _event_cameras) {
            if (event_camera && event_camera->is_valid()) {
                event_camera->stop();
            }
        }
    }
}

void EventCameraModule::_initialize_event_cameras() {
    for (const EventCameraConfig& event_camera_config : _config.get_config().event_cameras) {
        spdlog::info("Initializing prophesee event camera: {}", event_camera_config.name);
        try {
            _event_cameras.push_back(create_event_camera(event_camera_config));
        } catch (const std::exception& error) {
            // NOTE: A single unreachable/broken event camera must not prevent the rest
            // of the module (and the other event cameras) from starting up.
            spdlog::error("Failed to initialize event camera '{}': {}", event_camera_config.name, error.what());
            _event_cameras.push_back(nullptr);
        }
    }
}

} // namespace core
