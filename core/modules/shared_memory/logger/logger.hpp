#ifndef WORKSPACES_CORE_CORE_MODULES_SHARED_MEMORY_LOGGER_HPP
#define WORKSPACES_CORE_CORE_MODULES_SHARED_MEMORY_LOGGER_HPP

#include <mcap/writer.hpp>

#include "../client/reader.hpp"
#include "configs.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace core {

struct CameraLogStream {
    std::string name;
    uint32_t num_frames{1};
    size_t width{0};
    size_t height{0};
    std::unique_ptr<SharedDictReader> reader;
    mcap::ChannelId channel_id{0};
    bool initialized{false};
    uint32_t last_sequence{0};
};

struct IMULogStream {
    std::string name;
    uint32_t num_frames{1};
    std::unique_ptr<SharedDictReader> reader;
    mcap::ChannelId channel_id{0};
    bool initialized{false};
    uint32_t last_sequence{0};
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
    std::string _output_path;
    std::unordered_map<std::string, uint32_t> _buffer_sizes;
    std::vector<CameraLogStream> _camera_streams;
    std::vector<IMULogStream> _imu_streams;

    void _drain_camera_stream(CameraLogStream& stream);
    void _drain_imu_stream(IMULogStream& stream);
    void _initialize_streams();
    void _open_writer();
    void _register_channels();

    template <typename T>
    void _append_value(std::vector<std::byte>& out, const T& value);
};

} // namespace core

#endif // WORKSPACES_CORE_CORE_MODULES_SHARED_MEMORY_LOGGER_HPP
