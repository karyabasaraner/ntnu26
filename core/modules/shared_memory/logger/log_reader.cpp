#include "log_reader.hpp"
#include "mcap/types.hpp"
#include "schema.hpp"

#include <array>
#include <exception>
#include <mcap/reader.hpp>

#include <algorithm>
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
#include <system_error>
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

void update_topic_metadata(
    TopicMetadata& metadata,
    uint64_t timestamp_ns,
    uint64_t payload_bytes,
    const std::string& timestamp_source,
    const std::string& timestamp_clock_domain,
    const std::string& timestamp_quality
) {
    if (metadata.message_count == 0) {
        metadata.start_time_ns = timestamp_ns;
        metadata.end_time_ns = timestamp_ns;
        metadata.timestamp_source = timestamp_source;
        metadata.timestamp_clock_domain = timestamp_clock_domain;
        metadata.timestamp_quality = timestamp_quality;
    } else {
        metadata.start_time_ns = std::min(metadata.start_time_ns, timestamp_ns);
        metadata.end_time_ns = std::max(metadata.end_time_ns, timestamp_ns);
        if (metadata.timestamp_source != timestamp_source) {
            metadata.timestamp_source = "mixed";
        }
        if (metadata.timestamp_clock_domain != timestamp_clock_domain) {
            metadata.timestamp_clock_domain = "mixed";
        }
        if (metadata.timestamp_quality != timestamp_quality) {
            metadata.timestamp_quality = "mixed";
        }
    }

    metadata.uses_timestamp_fallback = metadata.uses_timestamp_fallback || timestamp_quality == "fallback";
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

        encoded.push_back(kBase64Alphabet[(triple >> 18U) & 0x3FU]);
        encoded.push_back(kBase64Alphabet[(triple >> 12U) & 0x3FU]);
        encoded.push_back(index + 1U < bytes.size() ? kBase64Alphabet[(triple >> 6U) & 0x3FU] : '=');
        encoded.push_back(index + 2U < bytes.size() ? kBase64Alphabet[triple & 0x3FU] : '=');
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

std::size_t find_json_field(const std::string& json, const std::string& field, const std::string& topic) {
    const auto key = "\"" + field + "\"";
    auto search_position = std::size_t{0};

    while (true) {
        const auto key_position = json.find(key, search_position);
        if (key_position == std::string::npos) {
            throw std::runtime_error(
                std::string("Missing JSON field '").append(field).append("' for topic '").append(topic).append("'")
            );
        }

        auto previous_position = key_position;
        bool has_previous_non_whitespace = false;
        while (previous_position > 0U) {
            --previous_position;
            if (std::isspace(static_cast<unsigned char>(json[previous_position])) == 0) {
                has_previous_non_whitespace = true;
                break;
            }
        }

        if (!has_previous_non_whitespace || (json[previous_position] != '{' && json[previous_position] != ',')) {
            search_position = key_position + key.size();
            continue;
        }

        auto colon_position = key_position + key.size();
        while (colon_position < json.size() && std::isspace(static_cast<unsigned char>(json[colon_position])) != 0) {
            ++colon_position;
        }
        if (colon_position >= json.size() || json[colon_position] != ':') {
            search_position = key_position + key.size();
            continue;
        }

        return colon_position + 1U;
    }
}

bool has_json_field(const std::string& json, const std::string& field) {
    const auto key = "\"" + field + "\"";
    return json.find(key) != std::string::npos;
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

std::string extract_optional_json_string(const std::string& json, const std::string& field, const std::string& topic, const std::string& fallback) {
    if (!has_json_field(json, field)) {
        return fallback;
    }
    return extract_json_string(json, field, topic);
}

uint64_t extract_json_uint64(const std::string& json, const std::string& field, const std::string& topic) {
    try {
        return std::stoull(extract_json_number_text(json, field, topic));
    } catch (const std::exception& ex) {
        throw std::runtime_error("Malformed JSON integer field '" + field + "' for topic '" + topic + "': " + ex.what());
    }
}

uint64_t extract_optional_json_uint64(const std::string& json, const std::string& field, const std::string& topic, uint64_t fallback) {
    if (!has_json_field(json, field)) {
        return fallback;
    }
    return extract_json_uint64(json, field, topic);
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
    const auto timestamp_ns = extract_timestamp_ns(json, topic);
    series.timestamp_ns.push_back(timestamp_ns);
    series.host_receive_timestamp_ns.push_back(extract_optional_json_uint64(json, "host_receive_timestamp_ns", topic, timestamp_ns));
    series.timestamp_source.push_back(extract_optional_json_string(json, "timestamp_source", topic, "unknown"));
    series.timestamp_clock_domain.push_back(extract_optional_json_string(json, "timestamp_clock_domain", topic, "unknown"));
    series.timestamp_quality.push_back(extract_optional_json_string(json, "timestamp_quality", topic, "unknown"));
    series.sequence.push_back(static_cast<uint32_t>(extract_json_uint64(json, "sequence", topic)));
    series.x.push_back(static_cast<float>(extract_json_double(json, "x", topic)));
    series.y.push_back(static_cast<float>(extract_json_double(json, "y", topic)));
    series.z.push_back(static_cast<float>(extract_json_double(json, "z", topic)));

    update_topic_metadata(
        metadata,
        series.timestamp_ns.back(),
        message.dataSize,
        series.timestamp_source.back(),
        series.timestamp_clock_domain.back(),
        series.timestamp_quality.back()
    );
}

void decode_compressed_image_message(
    LoggedCompressedImageSeries& series,
    TopicMetadata& metadata,
    const std::string& topic,
    const mcap::Message& message
) {
    const auto json = message_data_to_string(message, topic);

    series.topic = topic;
    const auto timestamp_ns = extract_timestamp_ns(json, topic);
    series.timestamp_ns.push_back(timestamp_ns);
    series.host_receive_timestamp_ns.push_back(extract_optional_json_uint64(json, "host_receive_timestamp_ns", topic, timestamp_ns));
    series.timestamp_source.push_back(extract_optional_json_string(json, "timestamp_source", topic, "unknown"));
    series.timestamp_clock_domain.push_back(extract_optional_json_string(json, "timestamp_clock_domain", topic, "unknown"));
    series.timestamp_quality.push_back(extract_optional_json_string(json, "timestamp_quality", topic, "unknown"));
    series.frame_id.push_back(extract_json_string(json, "frame_id", topic));
    series.format.push_back(extract_json_string(json, "format", topic));
    series.jpeg_data.push_back(base64_decode(extract_json_string(json, "data", topic), topic));

    update_topic_metadata(
        metadata,
        series.timestamp_ns.back(),
        message.dataSize,
        series.timestamp_source.back(),
        series.timestamp_clock_domain.back(),
        series.timestamp_quality.back()
    );
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

    // NOLINTNEXTLINE(misc-const-correctness): MCAP reader methods mutate parser state while opening and reading.
    mcap::McapReader reader;
    const auto open_status = reader.open(path);
    if (!open_status.ok()) {
        throw std::runtime_error("Failed to open MCAP log '" + path + "': " + std::string(open_status.message));
    }

    // NOLINTNEXTLINE(misc-const-correctness): populated by std::filesystem::file_size.
    std::error_code file_size_error;
    log_file._metadata.file_size_bytes = std::filesystem::file_size(path, file_size_error);
    if (file_size_error) {
        throw std::runtime_error("Failed to determine MCAP log size for '" + path + "': " + file_size_error.message());
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
            << topic_metadata.payload_bytes << " payload bytes, "
            << "timestamp_source=" << topic_metadata.timestamp_source << ", "
            << "timestamp_clock_domain=" << topic_metadata.timestamp_clock_domain << ", "
            << "timestamp_quality=" << topic_metadata.timestamp_quality << ", "
            << "uses_timestamp_fallback=" << (topic_metadata.uses_timestamp_fallback ? "true" : "false") << '\n';
    }

    return out.str();
}

} // namespace core
