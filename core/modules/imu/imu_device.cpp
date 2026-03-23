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
    _log_device_attrs();
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

std::vector<double> IMUDevice::_get_available_frequencies(struct iio_channel* channel) {
    const char* attr_name = "sampling_frequency_available";
    std::vector<char> buf_char(256);
    int ret = iio_channel_attr_read(channel, attr_name, buf_char.data(), buf_char.size());
    if (ret < 0) {
        spdlog::error("Failed to read {}: {}", attr_name, std::strerror(-ret));
        return {};
    }

    const std::string attr_value(buf_char.data(), static_cast<size_t>(ret));
    spdlog::info("{}: {}", attr_name, attr_value);

    std::istringstream iss(attr_value);
    std::vector<double> available_freqs;
    double frequency;
    while (iss >> frequency) {
        available_freqs.push_back(frequency);
    }

    return available_freqs;
}

void IMUDevice::_configure_device() {
    if (_config.sampling_frequency > 0.0) {
        for (const auto& [name, channel] : _channels) {
            std::vector<double> available_freqs = _get_available_frequencies(channel);
            spdlog::info("Available frequencies for channel '{}': {}", name, fmt::join(available_freqs, ", "));

            const double requested_freq = _config.sampling_frequency;
            const bool supported = std::any_of(available_freqs.begin(), available_freqs.end(),
            [requested_freq](double value) {
                return std::fabs(value - requested_freq) <= 1e-3;
            });

            if (!supported) {
                spdlog::warn("Requested sampling frequency {} Hz is not supported by channel '{}'", _config.sampling_frequency, name);
                continue;
            }





            // Write the sampling frequency to the channel attribute
            int ret = iio_channel_attr_write_double(channel, "sampling_frequency", _config.sampling_frequency);
            spdlog::info("Set sampling frequency for channel '{}': {} Hz (write result: {})", name, _config.sampling_frequency, ret);
        }
    }
}

void IMUDevice::_log_device_attrs() {
    const unsigned int device_attr_count = iio_device_get_attrs_count(_device);
    spdlog::info("{} has {} device attribute(s)", _config.device, device_attr_count);
    for (unsigned int idx = 0; idx < device_attr_count; ++idx) {
        const char* attr = iio_device_get_attr(_device, idx);
        if (attr != nullptr) {
            spdlog::info("Device attribute: {}", attr);
        }
    }

    for (const auto chn : _channels) {
        struct iio_channel* channel = chn.second;
        if (channel == nullptr) {
            continue;
        }

        const unsigned int channel_attr_count = iio_channel_get_attrs_count(channel);
        if (channel_attr_count == 0) {
            continue;
        }

        spdlog::info("Channel {} has {} attribute(s)", chn.first, channel_attr_count);
        for (unsigned int attr_idx = 0; attr_idx < channel_attr_count; ++attr_idx) {
            const char* attr = iio_channel_get_attr(channel, attr_idx);
            if (attr != nullptr) {
                spdlog::info("Channel attribute: {}", attr);
            }
        }
    }
}

} // namespace core
