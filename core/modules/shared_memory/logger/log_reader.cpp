#include "log_reader.hpp"

#include <mcap/reader.hpp>
#include <mcap/types.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace core {
namespace {

constexpr auto kImuSchemaName = "core/ImuXYZ";
constexpr auto kCompressedImageSchemaName = "core/CompressedImage";
constexpr double kNanosecondsPerSecond = 1'000'000'000.0;

template <typename T>
T read_value(const std::byte* data, std::size_t data_size, std::size_t offset, const std::string& topic) {
    if (data == nullptr || offset > data_size || sizeof(T) > data_size - offset) {
        throw std::runtime_error("Malformed MCAP payload for topic '" + topic + "'");
    }

    T value{};
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    std::memcpy(&value, data + offset, sizeof(T));
    return value;
}

void update_topic_metadata(
    TopicMetadata& metadata,
    uint64_t timestamp_ns,
    uint64_t payload_bytes
) {
    if (metadata.message_count == 0) {
        metadata.start_time_ns = timestamp_ns;
        metadata.end_time_ns = timestamp_ns;
    } else {
        metadata.start_time_ns = std::min(metadata.start_time_ns, timestamp_ns);
        metadata.end_time_ns = std::max(metadata.end_time_ns, timestamp_ns);
    }

    ++metadata.message_count;
    metadata.payload_bytes += payload_bytes;
}

void finalize_topic_metadata(TopicMetadata& metadata) {
    if (metadata.message_count == 0) {
        return;
    }

    metadata.duration_s = static_cast<double>(metadata.end_time_ns - metadata.start_time_ns) / kNanosecondsPerSecond;
    if (metadata.message_count > 1 && metadata.duration_s > 0.0) {
        metadata.average_rate_hz = static_cast<double>(metadata.message_count - 1U) / metadata.duration_s;
    }
}

void finalize_log_metadata(LogMetadata& metadata) {
    uint64_t start_time_ns = std::numeric_limits<uint64_t>::max();
    uint64_t end_time_ns = 0;
    bool has_messages = false;

    for (auto& [unused_topic, topic_metadata] : metadata.topics) {
        static_cast<void>(unused_topic);
        finalize_topic_metadata(topic_metadata);
        if (topic_metadata.message_count == 0) {
            continue;
        }
        has_messages = true;
        start_time_ns = std::min(start_time_ns, topic_metadata.start_time_ns);
        end_time_ns = std::max(end_time_ns, topic_metadata.end_time_ns);
    }

    if (!has_messages) {
        return;
    }

    metadata.start_time_ns = start_time_ns;
    metadata.end_time_ns = end_time_ns;
    metadata.duration_s = static_cast<double>(end_time_ns - start_time_ns) / kNanosecondsPerSecond;
}

TopicMetadata& ensure_metadata(
    LogMetadata& log_metadata,
    const std::string& topic,
    LogTopicType type
) {
    auto [metadata_it, inserted] = log_metadata.topics.try_emplace(topic);
    if (inserted) {
        metadata_it->second.topic = topic;
        metadata_it->second.type = type;
    }
    return metadata_it->second;
}

void decode_imu_message(LoggedImuSeries& series, TopicMetadata& metadata, const std::string& topic, const mcap::Message& message) {
    constexpr std::size_t timestamp_offset = 0;
    constexpr std::size_t sequence_offset = timestamp_offset + sizeof(uint64_t);
    constexpr std::size_t x_offset = sequence_offset + sizeof(uint32_t);
    constexpr std::size_t y_offset = x_offset + sizeof(float);
    constexpr std::size_t z_offset = y_offset + sizeof(float);
    constexpr std::size_t expected_size = z_offset + sizeof(float);

    if (message.dataSize != expected_size) {
        throw std::runtime_error("Malformed IMU MCAP payload for topic '" + topic + "'");
    }

    const auto data_size = static_cast<std::size_t>(message.dataSize);
    series.topic = topic;
    series.timestamp_ns.push_back(read_value<uint64_t>(message.data, data_size, timestamp_offset, topic));
    series.sequence.push_back(read_value<uint32_t>(message.data, data_size, sequence_offset, topic));
    series.x.push_back(read_value<float>(message.data, data_size, x_offset, topic));
    series.y.push_back(read_value<float>(message.data, data_size, y_offset, topic));
    series.z.push_back(read_value<float>(message.data, data_size, z_offset, topic));

    update_topic_metadata(metadata, series.timestamp_ns.back(), message.dataSize);
}

void decode_compressed_image_message(
    LoggedCompressedImageSeries& series,
    TopicMetadata& metadata,
    const std::string& topic,
    const mcap::Message& message
) {
    constexpr std::size_t timestamp_offset = 0;
    constexpr std::size_t sequence_offset = timestamp_offset + sizeof(uint64_t);
    constexpr std::size_t width_offset = sequence_offset + sizeof(uint32_t);
    constexpr std::size_t height_offset = width_offset + sizeof(uint32_t);
    constexpr std::size_t channels_offset = height_offset + sizeof(uint32_t);
    constexpr std::size_t jpeg_quality_offset = channels_offset + sizeof(uint8_t);
    constexpr std::size_t jpeg_size_offset = jpeg_quality_offset + sizeof(uint8_t);
    constexpr std::size_t jpeg_data_offset = jpeg_size_offset + sizeof(uint32_t);

    if (message.dataSize < jpeg_data_offset) {
        throw std::runtime_error("Malformed compressed image MCAP payload for topic '" + topic + "'");
    }

    const auto data_size = static_cast<std::size_t>(message.dataSize);
    const auto jpeg_size = read_value<uint32_t>(message.data, data_size, jpeg_size_offset, topic);
    if (jpeg_size > data_size - jpeg_data_offset) {
        throw std::runtime_error("Malformed compressed image MCAP payload for topic '" + topic + "'");
    }

    series.topic = topic;
    series.timestamp_ns.push_back(read_value<uint64_t>(message.data, data_size, timestamp_offset, topic));
    series.sequence.push_back(read_value<uint32_t>(message.data, data_size, sequence_offset, topic));
    series.width.push_back(read_value<uint32_t>(message.data, data_size, width_offset, topic));
    series.height.push_back(read_value<uint32_t>(message.data, data_size, height_offset, topic));
    series.channels.push_back(read_value<uint8_t>(message.data, data_size, channels_offset, topic));
    series.jpeg_quality.push_back(read_value<uint8_t>(message.data, data_size, jpeg_quality_offset, topic));
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    const auto* jpeg_begin = message.data + jpeg_data_offset;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    series.jpeg_data.emplace_back(jpeg_begin, jpeg_begin + jpeg_size);

    update_topic_metadata(metadata, series.timestamp_ns.back(), message.dataSize);
}

std::string topic_type_to_string(LogTopicType type) {
    switch (type) {
        case LogTopicType::IMU:
            return "imu";
        case LogTopicType::COMPRESSED_IMAGE:
            return "compressed_image";
    }
    return "unknown";
}

} // namespace

std::vector<std::string> LogFile::topics() const {
    std::vector<std::string> result;
    result.reserve(_metadata.topics.size());
    for (const auto& [topic, unused_metadata] : _metadata.topics) {
        static_cast<void>(unused_metadata);
        result.push_back(topic);
    }
    std::sort(result.begin(), result.end());
    return result;
}

std::vector<std::string> LogFile::imu_topics() const {
    std::vector<std::string> result;
    result.reserve(_imu_topics.size());
    for (const auto& [topic, unused_series] : _imu_topics) {
        static_cast<void>(unused_series);
        result.push_back(topic);
    }
    std::sort(result.begin(), result.end());
    return result;
}

std::vector<std::string> LogFile::camera_topics() const {
    std::vector<std::string> result;
    result.reserve(_camera_topics.size());
    for (const auto& [topic, unused_series] : _camera_topics) {
        static_cast<void>(unused_series);
        result.push_back(topic);
    }
    std::sort(result.begin(), result.end());
    return result;
}

const LoggedImuSeries* LogFile::get_imu_data(const std::string& topic) const {
    const auto topic_iter = _imu_topics.find(topic);
    if (topic_iter == _imu_topics.end()) {
        return nullptr;
    }
    return &topic_iter->second;
}

const LoggedCompressedImageSeries* LogFile::get_camera_data(const std::string& topic) const {
    const auto topic_iter = _camera_topics.find(topic);
    if (topic_iter == _camera_topics.end()) {
        return nullptr;
    }
    return &topic_iter->second;
}

const LogMetadata& LogFile::metadata() const {
    return _metadata;
}

LogFile read_log_file(const std::string& path) {
    // NOLINTNEXTLINE(misc-const-correctness): the log is populated incrementally while parsing MCAP messages.
    LogFile log_file;
    log_file._metadata.path = path;
    log_file._metadata.file_size_bytes = std::filesystem::file_size(path);

    // NOLINTNEXTLINE(misc-const-correctness): MCAP reader methods mutate parser state while opening and reading.
    mcap::McapReader reader;
    const auto open_status = reader.open(path);
    if (!open_status.ok()) {
        throw std::runtime_error("Failed to open MCAP log '" + path + "': " + std::string(open_status.message));
    }

    const auto summary_status = reader.readSummary(mcap::ReadSummaryMethod::AllowFallbackScan);
    if (!summary_status.ok()) {
        throw std::runtime_error("Failed to read MCAP summary for '" + path + "': " + std::string(summary_status.message));
    }

    for (const auto& view : reader.readMessages()) {
        if (view.channel == nullptr || view.schema == nullptr) {
            continue;
        }

        if (view.schema->name == kImuSchemaName) {
            auto& series = log_file._imu_topics.try_emplace(view.channel->topic).first->second;
            auto& metadata = ensure_metadata(log_file._metadata, view.channel->topic, LogTopicType::IMU);
            decode_imu_message(series, metadata, view.channel->topic, view.message);
        } else if (view.schema->name == kCompressedImageSchemaName) {
            auto& series = log_file._camera_topics.try_emplace(view.channel->topic).first->second;
            auto& metadata = ensure_metadata(log_file._metadata, view.channel->topic, LogTopicType::COMPRESSED_IMAGE);
            decode_compressed_image_message(series, metadata, view.channel->topic, view.message);
        }
    }

    finalize_log_metadata(log_file._metadata);
    return log_file;
}

std::string format_log_metadata(const LogMetadata& metadata) {
    std::ostringstream out;
    out << "Log file: " << metadata.path << '\n';
    out << "File size: " << metadata.file_size_bytes << " bytes\n";
    out << "Duration: " << metadata.duration_s << " s\n";
    out << "Topics: " << metadata.topics.size() << '\n';

    auto topics = std::vector<std::string>{};
    topics.reserve(metadata.topics.size());
    for (const auto& [topic, unused_topic_metadata] : metadata.topics) {
        static_cast<void>(unused_topic_metadata);
        topics.push_back(topic);
    }
    std::sort(topics.begin(), topics.end());

    for (const auto& topic : topics) {
        const auto& topic_metadata = metadata.topics.at(topic);
        out << "  " << topic << " [" << topic_type_to_string(topic_metadata.type) << "]: "
            << topic_metadata.message_count << " messages, "
            << topic_metadata.duration_s << " s, "
            << topic_metadata.average_rate_hz << " Hz, "
            << topic_metadata.payload_bytes << " payload bytes\n";
    }

    return out.str();
}

} // namespace core
