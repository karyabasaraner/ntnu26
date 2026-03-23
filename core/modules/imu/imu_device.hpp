#ifndef WORKSPACES_CORE_CORE_MODULES_IMU_IMU_DEVICE_HPP
#define WORKSPACES_CORE_CORE_MODULES_IMU_IMU_DEVICE_HPP

#include <atomic>
#include <chrono>
#include <cstddef>
#include <mutex>
#include <string>
#include <sys/types.h>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "../utils/configs.hpp"

extern "C" {
struct iio_context;
struct iio_device;
struct iio_channel;
struct iio_buffer;
}

namespace core {

struct IMUSample {
    int64_t timestamp_ns{0};
    std::unordered_map<std::string, double> values_si;
};

class IMUDevice {
public:
    explicit IMUDevice(IMUConfig config);
    ~IMUDevice();

    IMUDevice(const IMUDevice&) = delete;
    IMUDevice& operator=(const IMUDevice&) = delete;
    IMUDevice(IMUDevice&&) = delete;
    IMUDevice& operator=(IMUDevice&&) = delete;

    void start();
    void stop();
    bool is_running() const noexcept;
    bool read_latest_sample(IMUSample& sample);

private:
    IMUConfig _config;
    std::atomic<bool> _running{false};
    struct iio_context* _context{nullptr};
    struct iio_device* _device{nullptr};
    struct iio_buffer* _buffer{nullptr};
    struct iio_channel* _timestamp_channel{nullptr};
    std::unordered_map<std::string, struct iio_channel*> _channels;
    std::unordered_map<std::string, double> _channel_scales;
    std::unordered_map<std::string, double> _channel_offsets;

    bool _open_context();
    void _prepare_channels();
    void _configure_device();
    bool _setup_buffer();
    void _destroy_resources();

    void _set_channel_attr(struct iio_channel* channel, const std::string& attr_name, double value);
    struct iio_channel* _find_channel_with_fallback(const std::string& configured_name, std::string& resolved_name) const;
    double _convert_channel_to_si(const std::string& configured_name, struct iio_channel* channel, const void* raw_ptr) const;
};

} // namespace core

#endif // WORKSPACES_CORE_CORE_MODULES_IMU_IMU_DEVICE_HPP
