#include "base_camera.hpp"

#include "configs.hpp"
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <system_error>
#include <utility>

#include <spdlog/spdlog.h>

namespace core {

Camera::Camera(CameraConfig config) : _config(std::move(config)), _shdict_writer(_config.name, _config.writer) {}

bool Camera::start() {
    std::lock_guard<std::mutex> const lock(_state_mutex);
    if (_state != State::initialized || !is_valid()) {
        return false;
    }

    if (!_start_acquisition()) {
        return false;
    }

    _running = true;
    try {
        _worker = std::thread([this] {
            _capture_loop();
            _running = false;
        });
    } catch (const std::system_error& error) {
        _running = false;
        _stop_acquisition();
        spdlog::error("Failed to start capture thread for camera {}: {}", _config.device, error.what());
        return false;
    }

    _state = State::running;
    spdlog::info("Started streaming for camera: {}", _config.device);
    return true;
}

void Camera::stop() noexcept {
    std::lock_guard<std::mutex> const lock(_state_mutex);
    if (_state == State::stopped) {
        return;
    }

    spdlog::info("Stopping camera: {}", _config.device);
    _running = false;

    if (_state == State::running && !_stop_acquisition()) {
        spdlog::warn("Failed to stop acquisition for camera: {}", _config.device);
    }

    if (_worker.joinable()) {
        _worker.join();
    }
    spdlog::info("Stopped capture thread for camera: {}", _config.device);

    _post_stop();
    _state = State::stopped;
}

void Camera::process_frame(void* data, size_t length, uint32_t sequence, uint64_t timestamp_ns) {
    // NOTE: Copy as quickly as possible to free this thread for the next frame
    _shdict_writer.add(_config.name, data, length, sequence, timestamp_ns);
}

} // namespace core
