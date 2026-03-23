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

// struct ChannelCursor {
//     const std::string* name;
//     struct iio_channel* channel;
//     const char* first;
// };


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
    std::atomic<bool> _running{false};
    std::thread _worker;
    struct iio_context* _context{nullptr};
    struct iio_device* _device{nullptr};
    struct iio_buffer* _buffer{nullptr};
    int _buffer_poll_fd{-1};
    struct iio_channel* _timestamp_channel{nullptr};
    std::unordered_map<std::string, struct iio_channel*> _channels;
    std::unordered_map<std::string, double> _channel_scales;
    std::unordered_map<std::string, double> _channel_offsets;
    mutable std::mutex _sample_mutex;
    IMUSample _latest_sample;
    bool _has_latest_sample{false};

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
