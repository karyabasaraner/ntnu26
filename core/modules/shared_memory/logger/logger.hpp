#ifndef WORKSPACES_CORE_CORE_MODULES_SHARED_MEMORY_LOGGER_LOGGER_HPP
#define WORKSPACES_CORE_CORE_MODULES_SHARED_MEMORY_LOGGER_LOGGER_HPP

#include <mcap/writer.hpp>

#include "../client/reader.hpp"
#include "../utils.hpp"
#include "configs.hpp"
#include "mcap/types.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace core {

enum StreamType : uint8_t {
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

class SharedDictStreamProcessor {
public:
    class SteadyClockUnixTimeMapper {
    public:
        explicit SteadyClockUnixTimeMapper(int64_t steady_to_unix_offset_ns);

        [[nodiscard]] static SteadyClockUnixTimeMapper from_current_clocks();
        [[nodiscard]] uint64_t to_unix_time_ns(uint64_t steady_timestamp_ns) const;
        [[nodiscard]] int64_t steady_to_unix_offset_ns() const;

    private:
        int64_t _steady_to_unix_offset_ns{0};
    };

    SharedDictStreamProcessor(
        mcap::McapWriter& writer,
        std::mutex& writer_mutex,
        int jpeg_quality,
        SteadyClockUnixTimeMapper timestamp_mapper = SteadyClockUnixTimeMapper::from_current_clocks()
    );

    void process(SensorStream& stream);

private:
    mcap::McapWriter* _writer{nullptr};
    std::mutex* _writer_mutex{nullptr};
    int _jpeg_quality{90};
    SteadyClockUnixTimeMapper _timestamp_mapper;

    void _get_camera_payload(DataEntry& entry, const SensorStream& stream, uint64_t timestamp_ns, std::vector<std::byte>& payload);
    static void _get_imu_payload(const DataEntry& entry, uint64_t timestamp_ns, std::vector<std::byte>& payload);
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
    SharedDictStreamProcessor::SteadyClockUnixTimeMapper _timestamp_mapper;
    SharedDictStreamProcessor _stream_processor;

    void _initialize_streams();
    void _open_writer();
    void _register_channels();
};

} // namespace core

#endif // WORKSPACES_CORE_CORE_MODULES_SHARED_MEMORY_LOGGER_LOGGER_HPP
