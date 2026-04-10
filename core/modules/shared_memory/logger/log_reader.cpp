#include "log_reader.hpp"
#include "schema.hpp"
#include <opencv2/core/hal/interface.h>

#include <exception>
#include <mcap/reader.hpp>
#include <mcap/types.hpp>
#include <mcap/writer.hpp>
#include <opencv2/core/mat.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iterator>
#include <limits>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace core {
namespace {

constexpr double kNanosecondsPerSecond = 1'000'000'000.0;
constexpr uint64_t kNanosecondsPerSecondUint = 1'000'000'000ULL;
constexpr uint8_t kBase64DecodeInvalid = 255U;
constexpr uint8_t kBase64DecodePadding = 254U;
constexpr std::string_view kBase64Alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
constexpr auto kLegacyImuSchemaName = "core/ImuXYZ";
constexpr auto kLegacyCompressedImageSchemaName = "core/CompressedImage";
constexpr auto kLegacySchemaEncoding = "schema";
constexpr auto kLegacyMessageEncoding = "binary";

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

std::string json_escape(const std::string& value) {
    std::ostringstream out;
    for (const char character : value) {
        switch (character) {
            case '"':
                out << "\\\"";
                break;
            case '\\':
                out << "\\\\";
                break;
            case '\b':
                out << "\\b";
                break;
            case '\f':
                out << "\\f";
                break;
            case '\n':
                out << "\\n";
                break;
            case '\r':
                out << "\\r";
                break;
            case '\t':
                out << "\\t";
                break;
            default:
                out << character;
                break;
        }
    }
    return out.str();
}

void append_timestamp_json(std::ostringstream& out, uint64_t timestamp_ns) {
    out << R"("timestamp":{"sec":)" << (timestamp_ns / kNanosecondsPerSecondUint)
        << R"(,"nsec":)" << (timestamp_ns % kNanosecondsPerSecondUint) << '}';
}

std::string base64_encode(const std::vector<std::byte>& bytes) {
    std::string encoded;
    encoded.reserve(((bytes.size() + 2U) / 3U) * 4U);

    for (std::size_t index = 0; index < bytes.size(); index += 3U) {
        const auto octet_a = static_cast<uint32_t>(std::to_integer<uint8_t>(bytes.at(index)));
        const auto octet_b = index + 1U < bytes.size() ? static_cast<uint32_t>(std::to_integer<uint8_t>(bytes.at(index + 1U))) : 0U;
        const auto octet_c = index + 2U < bytes.size() ? static_cast<uint32_t>(std::to_integer<uint8_t>(bytes.at(index + 2U))) : 0U;
        const auto triple = (octet_a << 16U) | (octet_b << 8U) | octet_c;

        encoded.push_back(kBase64Alphabet.at((triple >> 18U) & 0x3FU));
        encoded.push_back(kBase64Alphabet.at((triple >> 12U) & 0x3FU));
        encoded.push_back(index + 1U < bytes.size() ? kBase64Alphabet.at((triple >> 6U) & 0x3FU) : '=');
        encoded.push_back(index + 2U < bytes.size() ? kBase64Alphabet.at(triple & 0x3FU) : '=');
    }

    return encoded;
}

void assign_payload(std::vector<std::byte>& payload, const std::string& json) {
    payload.reserve(json.size());
    std::transform(json.begin(), json.end(), std::back_inserter(payload), [](char character) {
        return static_cast<std::byte>(static_cast<unsigned char>(character));
    });
}

std::string message_data_to_string(const mcap::Message& message, const std::string& topic) {
    if (message.data == nullptr) {
        throw std::runtime_error("Malformed MCAP JSON payload for topic '" + topic + "'");
    }

    std::string payload;
    payload.reserve(message.dataSize);
    for (const auto byte : std::span<const std::byte>(message.data, message.dataSize)) {
        payload.push_back(static_cast<char>(std::to_integer<unsigned char>(byte)));
    }
    return payload;
}

template <typename T>
T read_legacy_value(const mcap::Message& message, std::size_t offset, const std::string& topic) {
    if (message.data == nullptr || offset > message.dataSize || sizeof(T) > message.dataSize - offset) {
        throw std::runtime_error("Malformed legacy MCAP payload for topic '" + topic + "'");
    }

    T value{};
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    std::memcpy(&value, message.data + offset, sizeof(T));
    return value;
}

std::vector<std::byte> read_legacy_bytes(const mcap::Message& message, std::size_t offset, std::size_t size, const std::string& topic) {
    if (message.data == nullptr || offset > message.dataSize || size > message.dataSize - offset) {
        throw std::runtime_error("Malformed legacy MCAP payload for topic '" + topic + "'");
    }

    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    const auto* begin = message.data + offset;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    return {begin, begin + size};
}

std::vector<std::byte> try_fix_legacy_jpeg_channel_order(const std::vector<std::byte>& jpeg_data, uint8_t jpeg_quality) {
    std::vector<uint8_t> encoded;
    encoded.reserve(jpeg_data.size());
    std::transform(jpeg_data.begin(), jpeg_data.end(), std::back_inserter(encoded), [](std::byte byte) {
        return std::to_integer<uint8_t>(byte);
    });

    const cv::Mat encoded_mat(1, static_cast<int>(encoded.size()), CV_8UC1, encoded.data());
    const cv::Mat decoded_bgr = cv::imdecode(encoded_mat, cv::IMREAD_COLOR);
    if (decoded_bgr.empty()) {
        return jpeg_data;
    }

    cv::Mat corrected_bgr;
    cv::cvtColor(decoded_bgr, corrected_bgr, cv::COLOR_BGR2RGB);
    std::vector<uint8_t> corrected_encoded;
    const std::vector<int> params{cv::IMWRITE_JPEG_QUALITY, static_cast<int>(jpeg_quality)};
    if (!cv::imencode(".jpg", corrected_bgr, corrected_encoded, params)) {
        return jpeg_data;
    }

    std::vector<std::byte> corrected;
    corrected.reserve(corrected_encoded.size());
    std::transform(corrected_encoded.begin(), corrected_encoded.end(), std::back_inserter(corrected), [](uint8_t value) {
        return static_cast<std::byte>(value);
    });
    return corrected;
}

std::string frame_id_from_camera_topic(const std::string& topic) {
    constexpr std::string_view camera_prefix = "/camera/";
    constexpr std::string_view camera_suffix = "/image/compressed";
    if (topic.starts_with(camera_prefix) && topic.ends_with(camera_suffix) && topic.size() > camera_prefix.size() + camera_suffix.size()) {
        return topic.substr(camera_prefix.size(), topic.size() - camera_prefix.size() - camera_suffix.size());
    }
    return topic;
}

std::vector<std::byte> make_foxglove_imu_payload(const mcap::Message& message, const std::string& topic) {
    constexpr std::size_t timestamp_offset = 0;
    constexpr std::size_t sequence_offset = timestamp_offset + sizeof(uint64_t);
    constexpr std::size_t x_offset = sequence_offset + sizeof(uint32_t);
    constexpr std::size_t y_offset = x_offset + sizeof(float);
    constexpr std::size_t z_offset = y_offset + sizeof(float);
    constexpr std::size_t expected_size = z_offset + sizeof(float);

    if (message.dataSize != expected_size) {
        throw std::runtime_error("Malformed legacy IMU MCAP payload for topic '" + topic + "'");
    }

    std::ostringstream json;
    json << '{';
    append_timestamp_json(json, read_legacy_value<uint64_t>(message, timestamp_offset, topic));
    json << R"(,"sequence":)" << read_legacy_value<uint32_t>(message, sequence_offset, topic)
         << R"(,"x":)" << read_legacy_value<float>(message, x_offset, topic)
         << R"(,"y":)" << read_legacy_value<float>(message, y_offset, topic)
         << R"(,"z":)" << read_legacy_value<float>(message, z_offset, topic)
         << '}';

    std::vector<std::byte> payload;
    assign_payload(payload, json.str());
    return payload;
}

std::vector<std::byte> make_foxglove_compressed_image_payload(const mcap::Message& message, const std::string& topic) {
    constexpr std::size_t timestamp_offset = 0;
    constexpr std::size_t sequence_offset = timestamp_offset + sizeof(uint64_t);
    constexpr std::size_t width_offset = sequence_offset + sizeof(uint32_t);
    constexpr std::size_t height_offset = width_offset + sizeof(uint32_t);
    constexpr std::size_t channels_offset = height_offset + sizeof(uint32_t);
    constexpr std::size_t jpeg_quality_offset = channels_offset + sizeof(uint8_t);
    constexpr std::size_t jpeg_size_offset = jpeg_quality_offset + sizeof(uint8_t);
    constexpr std::size_t jpeg_data_offset = jpeg_size_offset + sizeof(uint32_t);
    static_cast<void>(read_legacy_value<uint32_t>(message, width_offset, topic));
    static_cast<void>(read_legacy_value<uint32_t>(message, height_offset, topic));
    const auto channels = read_legacy_value<uint8_t>(message, channels_offset, topic);
    const auto jpeg_quality = read_legacy_value<uint8_t>(message, jpeg_quality_offset, topic);
    const auto jpeg_size = read_legacy_value<uint32_t>(message, jpeg_size_offset, topic);
    auto jpeg_data = read_legacy_bytes(message, jpeg_data_offset, jpeg_size, topic);
    if (channels == 3U) {
        jpeg_data = try_fix_legacy_jpeg_channel_order(jpeg_data, jpeg_quality);
    }

    std::ostringstream json;
    json << '{';
    append_timestamp_json(json, read_legacy_value<uint64_t>(message, timestamp_offset, topic));
    json << R"(,"frame_id":")" << json_escape(frame_id_from_camera_topic(topic))
         << R"(","data":")" << base64_encode(jpeg_data)
         << R"(","format":"jpeg"})";

    std::vector<std::byte> payload;
    assign_payload(payload, json.str());
    return payload;
}

void write_converted_message(mcap::McapWriter& writer, mcap::ChannelId channel_id, const mcap::Message& source_message, const std::vector<std::byte>& payload) {
    mcap::Message output_message;
    output_message.channelId = channel_id;
    output_message.sequence = source_message.sequence;
    output_message.publishTime = source_message.publishTime;
    output_message.logTime = source_message.logTime;
    output_message.data = payload.data();
    output_message.dataSize = payload.size();

    const auto write_status = writer.write(output_message);
    if (!write_status.ok()) {
        throw std::runtime_error("Failed to write converted MCAP message: " + std::string(write_status.message));
    }
}

std::size_t find_json_field(const std::string& json, const std::string& field, const std::string& topic) {
    const auto key = "\"" + field + "\"";
    const auto key_position = json.find(key);
    if (key_position == std::string::npos) {
        throw std::runtime_error("Missing JSON field '" + field + "' for topic '" + topic + "'");
    }

    const auto colon_position = json.find(':', key_position + key.size());
    if (colon_position == std::string::npos) {
        throw std::runtime_error("Malformed JSON field '" + field + "' for topic '" + topic + "'");
    }
    return colon_position + 1U;
}

std::size_t skip_whitespace(const std::string& json, std::size_t offset) {
    while (offset < json.size() && std::isspace(static_cast<unsigned char>(json[offset])) != 0) {
        ++offset;
    }
    return offset;
}

std::string extract_json_string(const std::string& json, const std::string& field, const std::string& topic) {
    auto offset = skip_whitespace(json, find_json_field(json, field, topic));
    if (offset >= json.size() || json[offset] != '"') {
        throw std::runtime_error("Malformed JSON string field '" + field + "' for topic '" + topic + "'");
    }
    ++offset;

    std::string value;
    while (offset < json.size()) {
        const auto character = json[offset++];
        if (character == '"') {
            return value;
        }
        if (character != '\\') {
            value.push_back(character);
            continue;
        }
        if (offset >= json.size()) {
            break;
        }
        const auto escaped = json[offset++];
        switch (escaped) {
            case '"':
            case '\\':
            case '/':
                value.push_back(escaped);
                break;
            case 'b':
                value.push_back('\b');
                break;
            case 'f':
                value.push_back('\f');
                break;
            case 'n':
                value.push_back('\n');
                break;
            case 'r':
                value.push_back('\r');
                break;
            case 't':
                value.push_back('\t');
                break;
            default:
                throw std::runtime_error(std::string("Unsupported JSON escape in field '").append(field).append("' for topic '").append(topic).append("'"));
        }
    }

    throw std::runtime_error("Unterminated JSON string field '" + field + "' for topic '" + topic + "'");
}

std::string extract_json_number_text(const std::string& json, const std::string& field, const std::string& topic) {
    auto offset = skip_whitespace(json, find_json_field(json, field, topic));
    const auto begin = offset;
    while (offset < json.size()) {
        const auto character = json[offset];
        if (std::isdigit(static_cast<unsigned char>(character)) == 0 && character != '-' && character != '+' &&
            character != '.' && character != 'e' && character != 'E') {
            break;
        }
        ++offset;
    }

    if (begin == offset) {
        throw std::runtime_error("Malformed JSON number field '" + field + "' for topic '" + topic + "'");
    }
    return json.substr(begin, offset - begin);
}

uint64_t extract_json_uint64(const std::string& json, const std::string& field, const std::string& topic) {
    try {
        return std::stoull(extract_json_number_text(json, field, topic));
    } catch (const std::exception& ex) {
        throw std::runtime_error("Malformed JSON integer field '" + field + "' for topic '" + topic + "': " + ex.what());
    }
}

double extract_json_double(const std::string& json, const std::string& field, const std::string& topic) {
    try {
        return std::stod(extract_json_number_text(json, field, topic));
    } catch (const std::exception& ex) {
        throw std::runtime_error("Malformed JSON number field '" + field + "' for topic '" + topic + "': " + ex.what());
    }
}

uint64_t extract_timestamp_ns(const std::string& json, const std::string& topic) {
    const auto sec = extract_json_uint64(json, "sec", topic);
    const auto nsec = extract_json_uint64(json, "nsec", topic);
    if (nsec >= kNanosecondsPerSecondUint) {
        throw std::runtime_error("Malformed JSON timestamp for topic '" + topic + "'");
    }
    return sec * kNanosecondsPerSecondUint + nsec;
}

std::array<uint8_t, 256> make_base64_decode_table() {
    std::array<uint8_t, 256> table{};
    table.fill(kBase64DecodeInvalid);

    for (uint8_t index = 0; index < 26U; ++index) {
        table.at(static_cast<std::size_t>('A') + index) = index;
        table.at(static_cast<std::size_t>('a') + index) = static_cast<uint8_t>(26U + index);
    }
    for (uint8_t index = 0; index < 10U; ++index) {
        table.at(static_cast<std::size_t>('0') + index) = static_cast<uint8_t>(52U + index);
    }
    table.at(static_cast<std::size_t>('+')) = 62U;
    table.at(static_cast<std::size_t>('/')) = 63U;
    table.at(static_cast<std::size_t>('=')) = kBase64DecodePadding;
    return table;
}

std::vector<std::byte> base64_decode(const std::string& encoded, const std::string& topic) {
    static const auto decode_table = make_base64_decode_table();
    if (encoded.size() % 4U != 0U) {
        throw std::runtime_error("Malformed base64 image data for topic '" + topic + "'");
    }

    std::vector<std::byte> decoded;
    decoded.reserve((encoded.size() / 4U) * 3U);

    for (std::size_t index = 0; index < encoded.size(); index += 4U) {
        const auto char_a = decode_table.at(static_cast<unsigned char>(encoded.at(index)));
        const auto char_b = decode_table.at(static_cast<unsigned char>(encoded.at(index + 1U)));
        const auto char_c = decode_table.at(static_cast<unsigned char>(encoded.at(index + 2U)));
        const auto char_d = decode_table.at(static_cast<unsigned char>(encoded.at(index + 3U)));
        if (char_a >= kBase64DecodePadding || char_b >= kBase64DecodePadding ||
            (char_c == kBase64DecodeInvalid) || (char_d == kBase64DecodeInvalid)) {
            throw std::runtime_error("Malformed base64 image data for topic '" + topic + "'");
        }
        if (char_c == kBase64DecodePadding && char_d != kBase64DecodePadding) {
            throw std::runtime_error("Malformed base64 image data for topic '" + topic + "'");
        }

        const auto triple = (static_cast<uint32_t>(char_a) << 18U) |
                            (static_cast<uint32_t>(char_b) << 12U) |
                            (char_c == kBase64DecodePadding ? 0U : static_cast<uint32_t>(char_c) << 6U) |
                            (char_d == kBase64DecodePadding ? 0U : static_cast<uint32_t>(char_d));
        decoded.push_back(static_cast<std::byte>((triple >> 16U) & 0xFFU));
        if (char_c != kBase64DecodePadding) {
            decoded.push_back(static_cast<std::byte>((triple >> 8U) & 0xFFU));
        }
        if (char_d != kBase64DecodePadding) {
            decoded.push_back(static_cast<std::byte>(triple & 0xFFU));
        }
    }

    return decoded;
}

void decode_imu_message(LoggedImuSeries& series, TopicMetadata& metadata, const std::string& topic, const mcap::Message& message) {
    const auto json = message_data_to_string(message, topic);

    series.topic = topic;
    series.timestamp_ns.push_back(extract_timestamp_ns(json, topic));
    series.sequence.push_back(static_cast<uint32_t>(extract_json_uint64(json, "sequence", topic)));
    series.x.push_back(static_cast<float>(extract_json_double(json, "x", topic)));
    series.y.push_back(static_cast<float>(extract_json_double(json, "y", topic)));
    series.z.push_back(static_cast<float>(extract_json_double(json, "z", topic)));

    update_topic_metadata(metadata, series.timestamp_ns.back(), message.dataSize);
}

void decode_compressed_image_message(
    LoggedCompressedImageSeries& series,
    TopicMetadata& metadata,
    const std::string& topic,
    const mcap::Message& message
) {
    const auto json = message_data_to_string(message, topic);

    series.topic = topic;
    series.timestamp_ns.push_back(extract_timestamp_ns(json, topic));
    series.frame_id.push_back(extract_json_string(json, "frame_id", topic));
    series.format.push_back(extract_json_string(json, "format", topic));
    series.jpeg_data.push_back(base64_decode(extract_json_string(json, "data", topic), topic));

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

        if (view.channel->messageEncoding != JsonMessageEncoding || view.schema->encoding != JsonSchemaEncoding) {
            continue;
        }

        if (view.schema->name == ImuSchemaName) {
            auto& series = log_file._imu_topics.try_emplace(view.channel->topic).first->second;
            auto& metadata = ensure_metadata(log_file._metadata, view.channel->topic, LogTopicType::IMU);
            decode_imu_message(series, metadata, view.channel->topic, view.message);
        } else if (view.schema->name == CompressedImageSchemaName) {
            auto& series = log_file._camera_topics.try_emplace(view.channel->topic).first->second;
            auto& metadata = ensure_metadata(log_file._metadata, view.channel->topic, LogTopicType::COMPRESSED_IMAGE);
            decode_compressed_image_message(series, metadata, view.channel->topic, view.message);
        }
    }

    finalize_log_metadata(log_file._metadata);
    return log_file;
}

void convert_legacy_log_file_to_foxglove(const std::string& input_path, const std::string& output_path) {
    // NOLINTNEXTLINE(misc-const-correctness): MCAP reader methods mutate parser state while opening and reading.
    mcap::McapReader reader;
    const auto open_status = reader.open(input_path);
    if (!open_status.ok()) {
        throw std::runtime_error("Failed to open legacy MCAP log '" + input_path + "': " + std::string(open_status.message));
    }

    const auto summary_status = reader.readSummary(mcap::ReadSummaryMethod::AllowFallbackScan);
    if (!summary_status.ok()) {
        throw std::runtime_error("Failed to read legacy MCAP summary for '" + input_path + "': " + std::string(summary_status.message));
    }

    // NOLINTNEXTLINE(misc-const-correctness): MCAP writer methods mutate writer state.
    mcap::McapWriter writer;
    // NOLINTNEXTLINE(misc-const-correctness): compression is assigned after construction.
    mcap::McapWriterOptions options("core");
    options.compression = mcap::Compression::Zstd;
    const auto writer_status = writer.open(output_path, options);
    if (!writer_status.ok()) {
        throw std::runtime_error("Failed to open converted MCAP log '" + output_path + "': " + std::string(writer_status.message));
    }

    // NOLINTNEXTLINE(misc-const-correctness): MCAP writer assigns schema ids through this object.
    mcap::Schema image_schema(CompressedImageSchemaName, JsonSchemaEncoding, CompressedImageSchema.data());
    writer.addSchema(image_schema);
    // NOLINTNEXTLINE(misc-const-correctness): MCAP writer assigns schema ids through this object.
    mcap::Schema imu_schema(ImuSchemaName, JsonSchemaEncoding, ImuSchema.data());
    writer.addSchema(imu_schema);

    // NOLINTNEXTLINE(misc-const-correctness): clang-tidy does not see mutations through try_emplace in this loop.
    std::unordered_map<mcap::ChannelId, mcap::ChannelId> converted_channels;
    for (const auto& view : reader.readMessages()) {
        if (view.channel == nullptr || view.schema == nullptr) {
            continue;
        }
        if (view.channel->messageEncoding != kLegacyMessageEncoding || view.schema->encoding != kLegacySchemaEncoding) {
            continue;
        }

        // NOLINTNEXTLINE(misc-const-correctness): assigned by the schema-specific converter branch below.
        std::vector<std::byte> payload;
        // NOLINTNEXTLINE(misc-const-correctness): assigned by the schema-specific converter branch below.
        mcap::SchemaId schema_id = 0;
        if (view.schema->name == kLegacyImuSchemaName) {
            payload = make_foxglove_imu_payload(view.message, view.channel->topic);
            schema_id = imu_schema.id;
        } else if (view.schema->name == kLegacyCompressedImageSchemaName) {
            payload = make_foxglove_compressed_image_payload(view.message, view.channel->topic);
            schema_id = image_schema.id;
        } else {
            continue;
        }

        const auto [channel_iter, inserted] = converted_channels.try_emplace(view.channel->id, 0);
        if (inserted) {
            // NOLINTNEXTLINE(misc-const-correctness): MCAP writer assigns channel ids through this object.
            mcap::Channel output_channel(view.channel->topic, JsonMessageEncoding, schema_id);
            writer.addChannel(output_channel);
            channel_iter->second = output_channel.id;
        }
        write_converted_message(writer, channel_iter->second, view.message, payload);
    }

    writer.close();
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
