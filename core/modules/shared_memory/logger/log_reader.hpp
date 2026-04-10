#ifndef WORKSPACES_CORE_CORE_MODULES_SHARED_MEMORY_LOGGER_LOG_READER_HPP
#define WORKSPACES_CORE_CORE_MODULES_SHARED_MEMORY_LOGGER_LOG_READER_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace core {

enum class LogTopicType : uint8_t {
    IMU,
    COMPRESSED_IMAGE,
};

struct LoggedImuSeries {
    std::string topic;
    std::vector<uint64_t> timestamp_ns;
    std::vector<uint32_t> sequence;
    std::vector<float> x;
    std::vector<float> y;
    std::vector<float> z;
};

struct LoggedCompressedImageSeries {
    std::string topic;
    std::vector<uint64_t> timestamp_ns;
    std::vector<std::string> frame_id;
    std::vector<std::string> format;
    std::vector<std::vector<std::byte>> jpeg_data;
};

struct TopicMetadata {
    std::string topic;
    LogTopicType type{LogTopicType::IMU};
    uint64_t message_count{0};
    uint64_t start_time_ns{0};
    uint64_t end_time_ns{0};
    double duration_s{0.0};
    double average_rate_hz{0.0};
    uint64_t payload_bytes{0};
};

struct LogMetadata {
    std::string path;
    uint64_t file_size_bytes{0};
    uint64_t start_time_ns{0};
    uint64_t end_time_ns{0};
    double duration_s{0.0};
    std::unordered_map<std::string, TopicMetadata> topics;
};

class LogFile {
public:
    [[nodiscard]] std::vector<std::string> topics() const;
    [[nodiscard]] std::vector<std::string> imu_topics() const;
    [[nodiscard]] std::vector<std::string> camera_topics() const;

    [[nodiscard]] const LoggedImuSeries* get_imu_data(const std::string& topic) const;
    [[nodiscard]] const LoggedCompressedImageSeries* get_camera_data(const std::string& topic) const;
    [[nodiscard]] const LogMetadata& metadata() const;

private:
    friend LogFile read_log_file(const std::string& path);

    std::unordered_map<std::string, LoggedImuSeries> _imu_topics;
    std::unordered_map<std::string, LoggedCompressedImageSeries> _camera_topics;
    LogMetadata _metadata;
};

[[nodiscard]] LogFile read_log_file(const std::string& path);
[[nodiscard]] std::string format_log_metadata(const LogMetadata& metadata);
void convert_legacy_log_file_to_foxglove(const std::string& input_path, const std::string& output_path);

} // namespace core

#endif // WORKSPACES_CORE_CORE_MODULES_SHARED_MEMORY_LOGGER_LOG_READER_HPP
