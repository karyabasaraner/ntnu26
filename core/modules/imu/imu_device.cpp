#include "imu_device.hpp"

#include <algorithm>
#include <cmath>
#include <cerrno>
#include <cstdint>
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

    if (!_setup_buffer()) {
        _destroy_resources();
        _running.store(false, std::memory_order_release);
    }
}

void IMUDevice::stop() {
    bool expected = true;
    if (!_running.compare_exchange_strong(expected, false)) {
        return;
    }

    _destroy_resources();
}

bool IMUDevice::is_running() const noexcept {
    return _running.load(std::memory_order_acquire);
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

        iio_channel_enable(channel);
        _channels[name] = channel;
    }

    if (_channels.empty()) {
        spdlog::error("No IMU data channels enabled for {}. Buffer acquisition requires at least one axis channel.", _config.device);
    }

    // Enable timestamp channel so buffered samples carry acquisition time.
    _timestamp_channel = iio_device_find_channel(_device, "timestamp", false);
    if (_timestamp_channel != nullptr) {
        iio_channel_enable(_timestamp_channel);
        spdlog::info("Enabled timestamp channel for {}", _config.device);
    } else {
        spdlog::warn("Timestamp channel not found for {}", _config.device);
    }
}

void IMUDevice::_set_channel_attr(struct iio_channel* channel, const std::string& attr_name, double value) {
    const int ret = iio_channel_attr_write_double(channel, attr_name.c_str(), value);
    if (ret < 0) {
        spdlog::error("Failed to set {} to {}: {}", attr_name, value, std::strerror(-ret));
        return;
    }
    spdlog::info("Set {}: {}", attr_name, value);
}

void IMUDevice::_configure_device() {
    if (_config.sampling_frequency > 0.0) {
        for (const auto& [name, channel] : _channels) {
            spdlog::info("Configuring channel {}", name);
            _set_channel_attr(channel, "sampling_frequency", _config.sampling_frequency);
            _set_channel_attr(channel, "scale", _config.scale);

            double scale = _config.scale;
            if (iio_channel_attr_read_double(channel, "scale", &scale) < 0) {
                scale = _config.scale;
            }

            double offset = 0.0;
            if (iio_channel_attr_read_double(channel, "offset", &offset) < 0) {
                offset = 0.0;
            }

            _channel_scales[name] = scale;
            _channel_offsets[name] = offset;
        }
    }
}

bool IMUDevice::read_latest_sample(IMUSample& sample) {
    if (!is_running()) {
        spdlog::warn("Not running!");
        return false;
    }

    if (_buffer == nullptr) {
        spdlog::error("IIO buffer not initialized for {}", _config.device);
        return false;
    }

    const ssize_t refill_ret = iio_buffer_refill(_buffer);
    if (refill_ret < 0) {
        spdlog::warn("Failed to refill IIO buffer for {}: {}", _config.device, std::strerror(-refill_ret));
        return false;
    }

    const char* const buffer_end = static_cast<const char*>(iio_buffer_end(_buffer));
    const ptrdiff_t step = iio_buffer_step(_buffer);
    if (step <= 0) {
        spdlog::error("Invalid buffer step {} for {}", step, _config.device);
        return false;
    }

    sample = IMUSample{};

    if (_timestamp_channel != nullptr) {
        const char* ptr = static_cast<const char*>(iio_buffer_first(_buffer, _timestamp_channel));
        if (ptr < buffer_end) {
            const char* last = ptr;
            for (const char* cur = ptr; cur < buffer_end; cur += step) {
                last = cur;
            }
            int64_t timestamp_value = 0;
            iio_channel_convert(_timestamp_channel, &timestamp_value, last);
            sample.timestamp_ns = timestamp_value;
        }
    }

    for (const auto& [name, channel] : _channels) {
        const char* ptr = static_cast<const char*>(iio_buffer_first(_buffer, channel));
        if (ptr >= buffer_end) {
            continue;
        }

        const char* last = ptr;
        for (const char* cur = ptr; cur < buffer_end; cur += step) {
            last = cur;
        }

        sample.values_si[name] = _convert_channel_to_si(name, channel, last);
    }

    return !sample.values_si.empty();
}

bool IMUDevice::_setup_buffer() {
    if (_buffer != nullptr) {
        return true;
    }

    if (_channels.empty()) {
        spdlog::error("Cannot create IIO buffer for {}: no enabled data channels", _config.device);
        return false;
    }

    const size_t sample_count = _config.buffer_samples > 0 ? _config.buffer_samples : 64;

    const long long watermark = 1;
    const int watermark_ret = iio_device_buffer_attr_write_longlong(_device, "watermark", watermark);
    if (watermark_ret < 0) {
        spdlog::warn("Failed to set buffer watermark={} for {}: {}", watermark, _config.device, std::strerror(-watermark_ret));
    } else {
        spdlog::info("Set buffer watermark={} for {}", watermark, _config.device);
    }

    _buffer = iio_device_create_buffer(_device, sample_count, _config.cyclic_buffer);
    if (_buffer == nullptr) {
        spdlog::error("Failed to create IIO buffer for {} (samples={}, cyclic={}): {}", _config.device, sample_count, _config.cyclic_buffer, std::strerror(errno));
        return false;
    }

    const int blocking_mode_ret = iio_buffer_set_blocking_mode(_buffer, true);
    if (blocking_mode_ret < 0) {
        spdlog::warn("Failed to set blocking mode for {} buffer: {}", _config.device, std::strerror(-blocking_mode_ret));
    }

    spdlog::info("Created IIO buffer for {} (samples={}, cyclic={})", _config.device, sample_count, _config.cyclic_buffer);
    return true;
}

void IMUDevice::_destroy_resources() {
    if (_buffer != nullptr) {
        iio_buffer_destroy(_buffer);
        _buffer = nullptr;
    }

    _channels.clear();
    _channel_scales.clear();
    _channel_offsets.clear();
    _timestamp_channel = nullptr;
    _device = nullptr;

    if (_context != nullptr) {
        iio_context_destroy(_context);
        _context = nullptr;
    }
}

double IMUDevice::_convert_channel_to_si(const std::string& configured_name, struct iio_channel* channel, const void* raw_ptr) const {
    int64_t host_value = 0;
    iio_channel_convert(channel, &host_value, raw_ptr);

    double scale = _config.scale;
    const auto scale_it = _channel_scales.find(configured_name);
    if (scale_it != _channel_scales.end()) {
        scale = scale_it->second;
    }

    double offset = 0.0;
    const auto offset_it = _channel_offsets.find(configured_name);
    if (offset_it != _channel_offsets.end()) {
        offset = offset_it->second;
    }

    return (static_cast<double>(host_value) + offset) * scale;
}

} // namespace core
