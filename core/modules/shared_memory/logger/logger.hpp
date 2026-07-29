#ifndef WORKSPACES_CORE_CORE_MODULES_SHARED_MEMORY_LOGGER_LOGGER_HPP
#define WORKSPACES_CORE_CORE_MODULES_SHARED_MEMORY_LOGGER_LOGGER_HPP

#include <mcap/writer.hpp>

#include "../client/reader.hpp"
#include "../utils.hpp"
#include "configs.hpp"
#include "mcap/types.hpp"
#include "steady_clock_unix_time_mapper.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <chrono>
#include <string>
#include <unordered_map>
#include <vector>

namespace core {

enum StreamType : uint8_t {
    CAMERA,
    IMU,
    EVENT_CAMERA,
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
    std::chrono::steady_clock::time_point last_status_log{std::chrono::steady_clock::time_point::min()};
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
    SteadyClockUnixTimeMapper _timestamp_mapper;

    void _initialize_streams();
    void _open_writer();
    void _register_channels();
    void _log_stream_status(SensorStream& stream, const DataEntry& latest_entry);
    bool _process_stream(SensorStream& stream);
    bool _write_entry(SensorStream& stream, const DataEntry& entry);
    bool _get_camera_payload(const DataEntry& entry, const SensorStream& stream, uint64_t timestamp_ns, std::vector<std::byte>& payload);
    static bool _get_imu_payload(const DataEntry& entry, uint64_t timestamp_ns, std::vector<std::byte>& payload);
    static bool _get_event_camera_payload(const DataEntry& entry, uint64_t timestamp_ns, std::vector<std::byte>& payload);
};

} // namespace core

#endif // WORKSPACES_CORE_CORE_MODULES_SHARED_MEMORY_LOGGER_LOGGER_HPP
