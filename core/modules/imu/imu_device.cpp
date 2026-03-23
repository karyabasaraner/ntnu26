#include "imu_device.hpp"

#include <algorithm>
#include <cmath>
#include <cerrno>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

#include <iio.h>
#include <spdlog/spdlog.h>

namespace core {

IMUDevice::IMUDevice(IMUConfig config) : _config(std::move(config)) {}

IMUDevice::~IMUDevice() {
    stop();
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
        _channels[name] = channel;
    }
}

void IMUDevice::_set_channel_attr(struct iio_channel* channel, const std::string& attr_name, double value) {
    const int ret = iio_channel_attr_write_double(channel, attr_name.c_str(), value);
    if (ret < 0) {
        spdlog::error("Failed to set {} to {}: {}", attr_name, value, std::strerror(-ret));
    }
    spdlog::info("Set {}: {}", attr_name, value);
}

void IMUDevice::_configure_device() {
    if (_config.sampling_frequency > 0.0) {
        for (const auto& [name, channel] : _channels) {
            spdlog::info("Configuring channel {}", name);
            _set_channel_attr(channel, "sampling_frequency", _config.sampling_frequency);
            _set_channel_attr(channel, "scale", _config.scale);
        }
    }
}

} // namespace core
