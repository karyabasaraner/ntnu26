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

namespace {

std::string join_channel_names(const std::vector<std::string>& names) {
    std::string joined;
    for (size_t i = 0; i < names.size(); ++i) {
        if (i > 0) {
            joined += ", ";
        }
        joined += names[i];
    }
    return joined;
}

} // namespace

IMUDevice::IMUDevice(IMUConfig config) : _config(std::move(config)) {}

IMUDevice::~IMUDevice() {
    stop();
    if (_buffer != nullptr) {
        iio_buffer_destroy(_buffer);
        _buffer = nullptr;
    }
    if (_context != nullptr) {
        iio_context_destroy(_context);
        _context = nullptr;
    }
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
    _create_buffer();
    _last_log_time = std::chrono::steady_clock::now();
    _reader_thread = std::thread(&IMUDevice::_run, this);
}

void IMUDevice::stop() {
    bool expected = true;
    if (!_running.compare_exchange_strong(expected, false)) {
        return;
    }

    if (_buffer != nullptr) {
        iio_buffer_cancel(_buffer);
    }

    if (_reader_thread.joinable()) {
        _reader_thread.join();
    }
}

bool IMUDevice::is_running() const noexcept {
    return _running.load(std::memory_order_acquire);
}

std::vector<double> IMUDevice::latest_sample() const {
    std::lock_guard<std::mutex> lock(_sample_mutex);
    return _latest_sample;
}

std::vector<std::string> IMUDevice::channel_names() const {
    return _channel_names;
}

std::chrono::steady_clock::time_point IMUDevice::last_sample_time() const {
    std::lock_guard<std::mutex> lock(_sample_mutex);
    return _last_sample_time;
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
    struct ChannelEntry {
        struct iio_channel* channel;
        long index;
        std::string id;
    };

    std::vector<ChannelEntry> entries;
    entries.reserve(_config.channels.size());

    for (const std::string& name : _config.channels) {
        struct iio_channel* channel = iio_device_find_channel(_device, name.c_str(), false);
        if (channel == nullptr) {
            spdlog::warn("Could not find {}", name);
            continue;

        }

        const long index = iio_channel_get_index(channel);
        const char* id = iio_channel_get_id(channel);
        spdlog::info("Found channel '{}' (index {}) for {}", id, index, _config.device);
        entries.push_back(ChannelEntry{channel, index, id});
    }

    // CLEANUP UNTIL HERE

    for (const auto& entry : entries) {
        iio_channel_enable(entry.channel);
        _channel_index[entry.channel] = _channel_names.size();
        _channel_names.push_back(entry.id);
    }

    _channels_per_frame = _channel_names.size();
    if (_channels_per_frame == 0) {
        throw std::runtime_error("No IMU channels enabled");
    }

    _working_sample.assign(_channels_per_frame, 0.0);
    _latest_sample.assign(_channels_per_frame, 0.0);

    spdlog::info("Enabled {} IMU channel(s): {}", _channels_per_frame, join_channel_names(_channel_names));
}

void IMUDevice::_configure_device() {
    if (_config.sampling_frequency > 0.0) {
        int ret = iio_device_attr_write_double(_device, "sampling_frequency", _config.sampling_frequency);
        if (ret < 0) {
            ret = -ret;
            spdlog::warn("Failed to set sampling_frequency on {}: {}",
                         _config.device, std::strerror(ret));
        } else {
            spdlog::info("Target IMU sampling frequency: {} Hz", _config.sampling_frequency);
        }
    }
}

void IMUDevice::_create_buffer() {
    if (_config.buffer_samples == 0) {
        throw std::runtime_error("IMU buffer_samples must be greater than zero");
    }

    _buffer = iio_device_create_buffer(_device, _config.buffer_samples, _config.cyclic_buffer);
    if (_buffer == nullptr) {
        throw std::runtime_error("Failed to create IIO buffer for " + _config.device);
    }

    iio_buffer_set_blocking_mode(_buffer, true);
    spdlog::info("Created {}-sample buffer for {}", _config.buffer_samples, _config.device);
}

void IMUDevice::_run() {
    while (_running.load(std::memory_order_relaxed)) {
        const ssize_t ret = iio_buffer_refill(_buffer);
        if (ret < 0) {
            if (ret == -EAGAIN) {
                continue;
            }
            const int error = static_cast<int>(-ret);
            spdlog::error("Failed to refill IMU buffer: {}", std::strerror(error));
            break;
        }

        _channels_seen = 0;
        if (iio_buffer_foreach_sample(_buffer, &IMUDevice::_sample_callback, this) < 0) {
            spdlog::error("Failed to parse IMU samples for {}", _config.device);
            break;
        }
    }

    spdlog::info("Stopped IMU reader for {}", _config.device);
}

void IMUDevice::_publish_sample() {
    const auto now = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> lock(_sample_mutex);
        _latest_sample = _working_sample;
        _last_sample_time = now;
    }

    _samples_since_last_log.fetch_add(1, std::memory_order_relaxed);
    const auto elapsed = now - _last_log_time;
    if (elapsed >= std::chrono::seconds(1)) {
        const size_t count = _samples_since_last_log.exchange(0, std::memory_order_relaxed);
        const double rate = static_cast<double>(count) / std::chrono::duration<double>(elapsed).count();
        spdlog::info("IMU {} running at ~{:.1f} Hz (target {:.0f} Hz)",
                     _config.device, rate, _config.sampling_frequency);
        _last_log_time = now;
    }
}

ssize_t IMUDevice::_sample_callback(const struct iio_channel* chn, void* src, size_t len, void* user_data) {
    auto* self = static_cast<IMUDevice*>(user_data);
    return self->_handle_sample(chn, src, len);
}

ssize_t IMUDevice::_handle_sample(const struct iio_channel* chn, void* src, size_t len) {
    const auto it = _channel_index.find(chn);
    if (it == _channel_index.end()) {
        return 0;
    }

    double value = 0.0;
    iio_channel_convert(chn, &value, src);
    _working_sample[it->second] = value;

    ++_channels_seen;
    if (_channels_seen >= _channels_per_frame) {
        _channels_seen = 0;
        _publish_sample();
    }

    return 0;
}

} // namespace core
