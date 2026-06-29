#include "base_camera.hpp"

#include <spdlog/spdlog.h>
#include "configs.hpp"
#include <utility>
#include <cstddef>
#include <cstdint>

namespace core {

Camera::Camera(CameraConfig config) : _config(std::move(config)), _shdict_writer(_config.name, _config.writer) {}

Camera::~Camera() {
    if (_running) {
        stop();
    }
}

bool Camera::start() {
    if (!_start_acquisition()) {
        return false;
    }

    _running = true;
    spdlog::info("Started streaming for camera: {}", _config.device);
    _worker = std::thread(&Camera::_capture_loop, this);

    return true;
}

void Camera::stop() {
    spdlog::info("Stopping camera: {}", _config.device);
    _running = false;

    if (!_stop_acquisition()) {
        spdlog::warn("Failed to stop acquisition for camera: {}", _config.device);
    }

    if (_worker.joinable()) {
        _worker.join();
    }
    spdlog::info("Stopped capture thread for camera: {}", _config.device);

    // TODO(MJ): Is this the nicest way to do this?
    _post_stop();
}

void Camera::process_frame(void* data, size_t length, uint32_t sequence, uint64_t timestamp_ns) {
    // NOTE: Copy as quickly as possible to free this thread for the next frame
    _shdict_writer.add(_config.name, data, length, sequence, timestamp_ns);
}

bool Camera::_start_acquisition() {
    // Empty implementation in base class; can be overridden in derived classes for pre-start actions
    return true;
}

bool Camera::_stop_acquisition() {
    // Empty implementation in base class; can be overridden in derived classes for post-stop actions
    return true;
}



} // namespace core
