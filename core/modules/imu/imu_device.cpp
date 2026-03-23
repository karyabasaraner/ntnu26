#include "imu_device.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <stdexcept>
#include <string>
#include <string_view>

#include <iio.h>
#include <spdlog/spdlog.h>

namespace core {

IMUDevice::IMUDevice(IMUConfig config) : _config(std::move(config)) {}

IMUDevice::~IMUDevice() {
    stop();
    // if (_buffer != nullptr) {
    //     iio_buffer_destroy(_buffer);
    //     _buffer = nullptr;
    // }
    // if (_context != nullptr) {
    //     iio_context_destroy(_context);
    //     _context = nullptr;
    // }
}

void IMUDevice::start() {
    bool expected = false;
    if (!_running.compare_exchange_strong(expected, true)) {
        spdlog::warn("IMU device {} already running", _config.device);
        return;
    }

    if (!_open_context()) {
        _running.store(false, std::memory_order_release);
        return;
    }

    _prepare_channels();
    _configure_device();
}

void IMUDevice::stop() {
    bool expected = true;
    if (!_running.compare_exchange_strong(expected, false)) {
        return;
    }
}

bool IMUDevice::_open_context() {
    _context = iio_create_default_context();
    if (_context == nullptr) {
        spdlog::error("Failed to create IIO context for IMU device '{}'", _config.device);
        return false;
    }

    _device = iio_context_find_device(_context, _config.device.c_str());
    if (_device == nullptr) {
        iio_context_destroy(_context);
        _context = nullptr;
        spdlog::error("IIO device '{}' not found in context", _config.device);
        return false;
    }

    spdlog::info("Found IMU device {}", iio_device_get_id(_device));
    return true;
}

void IMUDevice::_prepare_channels() {
    for (const std::string& name : _config.channels) {
        struct iio_channel* channel = iio_device_find_channel(_device, name.c_str(), false);
        if (channel == nullptr) {
            spdlog::warn("Could not find {}", name);
            continue;
        }

        // Get channel index and ID
        const int64_t index = iio_channel_get_index(channel);
        const char* iden = iio_channel_get_id(channel);
        spdlog::info("Found channel '{}' (index {}) for {}", iden, index, _config.device);

        // Enable channel
        iio_channel_enable(channel);

        // Store channel and name for later use
        _channels[name] = std::make_pair(index, channel);
    }
}

void IMUDevice::_configure_device() {
    if (_config.sampling_frequency > 0.0) {
        int ret = iio_device_attr_write_double(_device, "sampling_frequency", _config.sampling_frequency);
        if (ret < 0) {
            ret = -ret;
            spdlog::warn("Failed to set sampling_frequency on {}: {}", _config.device, std::strerror(ret));
        } else {
            spdlog::info("Target IMU sampling frequency: {} Hz", _config.sampling_frequency);
        }
    }
}

} // namespace core
