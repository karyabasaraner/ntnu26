#ifndef WORKSPACES_CORE_CORE_MODULES_IMU_IMU_DEVICE_HPP
#define WORKSPACES_CORE_CORE_MODULES_IMU_IMU_DEVICE_HPP

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
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
    std::vector<uint64_t> timestamps_ns;
    std::vector<std::vector<float>> data;
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

private:
    IMUConfig _config;
    int _buffer_poll_fd{-1};
    std::atomic<bool> _running{false};
    std::thread _worker;
    std::unordered_map<std::string, float> _channel_offsets;
    std::unordered_map<std::string, float> _channel_scales;
    std::unordered_map<std::string, struct iio_channel*> _channels;
    struct iio_buffer* _buffer{nullptr};
    struct iio_channel* _timestamp_channel{nullptr};
    struct iio_context* _context{nullptr};
    struct iio_device* _device{nullptr};

    bool _open_context();
    bool _setup_buffer();
    size_t _read_buffer_sample(IMUSample& latest_sample);
    void _capture_loop();
    void _configure_device();
    void _destroy_resources();
    void _prepare_channels();
    void _set_channel_attr(struct iio_channel* channel, const std::string& attr_name, double value);
};

} // namespace core

#endif // WORKSPACES_CORE_CORE_MODULES_IMU_IMU_DEVICE_HPP
