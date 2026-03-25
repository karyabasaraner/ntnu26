#ifndef WORKSPACES_CORE_CORE_MODULES_SHARED_MEMORY_LOGGER_HPP
#define WORKSPACES_CORE_CORE_MODULES_SHARED_MEMORY_LOGGER_HPP

#include <mcap/writer.hpp>

#include "../client/reader.hpp"
#include "configs.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace core {

enum StreamType {
    CAMERA,
    IMU,
};

struct SensorStream {
    bool initialized{false};
    mcap::ChannelId channel_id{0};
    size_t height{0};
    size_t width{0};
    std::string name;
    std::unique_ptr<SharedDictReader> reader;
    StreamType type;
    uint32_t current_head{0};
    uint32_t current_sequence{0};
    uint32_t num_frames{1};

};

class SharedDictLogger {
public:
    SharedDictLogger(const std::string& config_path, std::string output_path);
    ~SharedDictLogger();

    SharedDictLogger(const SharedDictLogger&) = delete;
    SharedDictLogger(SharedDictLogger&&) = delete;
    SharedDictLogger& operator=(const SharedDictLogger&) = delete;
    SharedDictLogger& operator=(SharedDictLogger&&) = delete;

    void run();
    void request_stop();

private:
    Config _config;
    int _jpeg_quality{90};
    mcap::McapWriter _writer;
    std::atomic<bool> _stop{false};
    std::mutex _writer_mutex;
    std::string _output_path;
    std::unordered_map<std::string, uint32_t> _buffer_sizes;
    std::vector<SensorStream> _sensor_streams;

    void _process_sensor_stream(SensorStream& stream);
    void _initialize_streams();
    void _open_writer();
    void _register_channels();
    void _get_camera_payload(DataEntry& entry, const SensorStream& stream, std::vector<std::byte>& payload);
    void _get_imu_payload(const DataEntry& entry, std::vector<std::byte>& payload);

    template <typename T>
    void _append_value(std::vector<std::byte>& out, const T& value);
};

} // namespace core

#endif // WORKSPACES_CORE_CORE_MODULES_SHARED_MEMORY_LOGGER_HPP
