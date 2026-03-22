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

    std::vector<double> latest_sample() const;
    std::vector<std::string> channel_names() const;
    std::chrono::steady_clock::time_point last_sample_time() const;

private:
    bool _open_context();
    void _prepare_channels();
    void _configure_device();
    void _create_buffer();
    void _run();
    void _publish_sample();

    static ssize_t _sample_callback(const struct iio_channel* chn, void* src, size_t len,
                                     void* user_data);
    ssize_t _handle_sample(const struct iio_channel* chn, void* src, size_t len);

    IMUConfig _config;
    struct iio_context* _context{nullptr};
    struct iio_device* _device{nullptr};
    struct iio_buffer* _buffer{nullptr};

    std::vector<double> _working_sample;
    std::vector<double> _latest_sample;
    std::vector<std::string> _channel_names;
    std::unordered_map<const struct iio_channel*, size_t> _channel_index;
    size_t _channels_per_frame{0};

    mutable std::mutex _sample_mutex;
    std::chrono::steady_clock::time_point _last_sample_time{};
    mutable std::chrono::steady_clock::time_point _last_log_time{};
    std::atomic<size_t> _samples_since_last_log{0};

    std::thread _reader_thread;
    std::atomic<bool> _running{false};
    size_t _channels_seen{0};
};

} // namespace core

#endif // WORKSPACES_CORE_CORE_MODULES_IMU_IMU_DEVICE_HPP
