#include "imu_device.hpp"

#include "configs.hpp"
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iio.h>
#include <poll.h>
#include <spdlog/spdlog.h>
#include <string>
#include <sys/poll.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace core {

IMUDevice::IMUDevice(IMUConfig config) : _config(std::move(config)), _shdict_writer(_config.name, _config.writer) {}

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
        return;
    }

    _worker = std::thread(&IMUDevice::_capture_loop, this);
}

void IMUDevice::stop() {
    bool expected = true;
    if (!_running.compare_exchange_strong(expected, false)) {
        return;
    }

    if (_buffer != nullptr) {
        // Wake any blocking refill so the worker can exit promptly.
        iio_buffer_cancel(_buffer);
    }

    if (_worker.joinable()) {
        _worker.join();
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
        spdlog::error("Failed to set {} to {}: {}", attr_name, value, -ret);
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

            auto scale = static_cast<double>(_config.scale);
            if (iio_channel_attr_read_double(channel, "scale", &scale) < 0) {
                scale = _config.scale;
            }

            double offset = 0.0;
            if (iio_channel_attr_read_double(channel, "offset", &offset) < 0) {
                offset = 0.0;
            }

            _channel_scales[name] = static_cast<float>(scale);
            _channel_offsets[name] = static_cast<float>(offset);
        }
    }
}

void IMUDevice::_capture_loop() {
    while (_running.load(std::memory_order_acquire)) {
        if (_buffer_poll_fd >= 0) {
            struct pollfd pfd{};
            pfd.fd = _buffer_poll_fd;
            pfd.events = POLLIN;
            const int poll_ret = poll(&pfd, 1, 250); // 250 ms
            if (poll_ret < 0) {
                spdlog::warn("Poll error on IIO buffer for {}: {}", _config.device, errno);
                continue;

            }
            if (poll_ret == 0) {
                // This is ok
                continue;
            }
        }
        _read_and_process_samples();
    }
}

void IMUDevice::_read_and_process_samples() {
    // 0. Check if buffer is initialized
    if (_buffer == nullptr) {
        spdlog::error("IIO buffer not initialized for {}", _config.device);
        return;
    }

    // 1. Refill buffer with new samples from device
    const auto timestamp = std::chrono::steady_clock::now();
    const ssize_t refill_ret = iio_buffer_refill(_buffer);
    if (refill_ret <= 0) {
        spdlog::warn("Failed to refill IIO buffer for {}: {}", _config.device, -refill_ret);
        return;
    }

    // 2. Get the buffer step size
    const ptrdiff_t step = iio_buffer_step(_buffer);
    const size_t num_samples_in_buffer = static_cast<size_t>(refill_ret) / static_cast<size_t>(step);

    // 3. Iterate over samples in buffer and decode them
    auto offset_imu_real_ns = std::chrono::nanoseconds(0);
    for (size_t index = 0; index < num_samples_in_buffer; ++index) {
        // 3a. Decode timestamp
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
        const char* timestamp_ptr = static_cast<const char*>(iio_buffer_first(_buffer, _timestamp_channel)) + static_cast<ptrdiff_t>(index) * step;
        if (timestamp_ptr >= static_cast<const char*>(iio_buffer_end(_buffer))) {
            spdlog::warn("Timestamp pointer out of buffer bounds for sample {}", index);
            continue;
        }

        int64_t timestamp_imu = 0;
        iio_channel_convert(_timestamp_channel, &timestamp_imu, timestamp_ptr);

        if (index == 0) {
            // Record a new offset at the first sample
            offset_imu_real_ns = std::chrono::nanoseconds(timestamp_imu) - timestamp.time_since_epoch();
        }
        const auto timestamp_real = std::chrono::nanoseconds(timestamp_imu) - offset_imu_real_ns;

        // 3b. Decode data channels
        std::vector<float> channel_data;
        channel_data.reserve(_channels.size());
        for (const auto& [name, channel] : _channels) {
            // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
            const char* channel_ptr = static_cast<const char*>(iio_buffer_first(_buffer, channel)) + static_cast<ptrdiff_t>(index) * step;
            if (channel_ptr >= static_cast<const char*>(iio_buffer_end(_buffer))) {
                spdlog::warn("Channel {} pointer out of buffer bounds for sample {}", name, index);
                continue;
            }

            int16_t value = 0;
            iio_channel_convert(channel, &value, channel_ptr);

            // Convert to SI units using scale and offset
            const float value_si = (static_cast<float>(value) + _channel_offsets[name]) * _channel_scales[name];
            channel_data.push_back(value_si);
        }

        // 3c. Reverse the buffer from zyx to xyz order
        std::reverse(channel_data.begin(), channel_data.end());

        // 3c. Submit to writer
        const size_t length = channel_data.size() * sizeof(float);
        _shdict_writer.add(_config.name, channel_data.data(), length, _sequence, timestamp_real.count());
        _sequence++;
    }
}

bool IMUDevice::_setup_buffer() {
    if (_buffer != nullptr) {
        return true;
    }

    if (_channels.empty()) {
        spdlog::error("Cannot create IIO buffer for {}: no enabled data channels", _config.device);
        return false;
    }

    // Set watermark
    // NOLINTNEXTLINE(google-runtime-int)
    const auto watermark_samples = static_cast<long long>(_config.watermark_samples);
    const int watermark_ret = iio_device_buffer_attr_write_longlong(_device, "watermark", watermark_samples);
    if (watermark_ret < 0) {
        spdlog::warn("Failed to set buffer watermark={} for {}: {}", watermark_samples, _config.device, -watermark_ret);
    } else {
        spdlog::info("Set buffer watermark={} for {}", watermark_samples, _config.device);
    }

    const size_t buffer_samples = _config.buffer_samples;
    _buffer = iio_device_create_buffer(_device, buffer_samples, false);
    if (_buffer == nullptr) {
        spdlog::error("Failed to create IIO buffer for {} (samples={}): {}", _config.device, buffer_samples, errno);
        return false;
    }

    // Get the poll fd for the buffer so we can wait for new samples efficiently
    _buffer_poll_fd = iio_buffer_get_poll_fd(_buffer);
    if (_buffer_poll_fd < 0) {
        spdlog::warn("Could not get poll fd for {} buffer, falling back to refill loop", _config.device);
    }

    // Set blocking mode
    const int blocking_mode_ret = iio_buffer_set_blocking_mode(_buffer, true);
    if (blocking_mode_ret < 0) {
        spdlog::warn("Failed to set blocking mode for {} buffer: {}", _config.device, -blocking_mode_ret);
    } else {
        spdlog::info("Set blocking mode for {} buffer", _config.device);
    }

    spdlog::info("Created IIO buffer for {} (samples={})", _config.device, buffer_samples);
    return true;
}

void IMUDevice::_destroy_resources() {
    if (_buffer != nullptr) {
        iio_buffer_destroy(_buffer);
        _buffer = nullptr;
    }
    _buffer_poll_fd = -1;

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

} // namespace core
