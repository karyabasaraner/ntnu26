#include "logger.hpp"
#include "../client/reader.hpp"
#include "../utils.hpp"
#include "mcap/types.hpp"
#include "mcap/writer.hpp"
#include "schema.hpp"
#include "steady_clock_unix_time_mapper.hpp"
#include "stream_progress.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <iterator>
#include <memory>
#include <mutex>
#include <opencv2/core/hal/interface.h>
#include <opencv2/core/mat.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <spdlog/spdlog.h>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace core {
namespace {

constexpr uint64_t kNanosecondsPerSecond = 1'000'000'000ULL;
constexpr auto kStatusLogInterval = std::chrono::seconds(10);
constexpr std::string_view kBase64Alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

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

std::string base64_encode(const std::vector<uint8_t>& bytes) {
    std::string encoded;
    encoded.reserve(((bytes.size() + 2U) / 3U) * 4U);

    for (std::size_t index = 0; index < bytes.size(); index += 3U) {
        const auto octet_a = static_cast<uint32_t>(bytes[index]);
        const auto octet_b = index + 1U < bytes.size() ? static_cast<uint32_t>(bytes[index + 1U]) : 0U;
        const auto octet_c = index + 2U < bytes.size() ? static_cast<uint32_t>(bytes[index + 2U]) : 0U;
        const auto triple = (octet_a << 16U) | (octet_b << 8U) | octet_c;

        encoded.push_back(kBase64Alphabet[(triple >> 18U) & 0x3FU]);
        encoded.push_back(kBase64Alphabet[(triple >> 12U) & 0x3FU]);
        encoded.push_back(index + 1U < bytes.size() ? kBase64Alphabet[(triple >> 6U) & 0x3FU] : '=');
        encoded.push_back(index + 2U < bytes.size() ? kBase64Alphabet[triple & 0x3FU] : '=');
    }

    return encoded;
}

void append_timestamp_json(std::ostringstream& out, uint64_t timestamp_ns) {
    out << R"("timestamp":{"sec":)" << (timestamp_ns / kNanosecondsPerSecond)
        << R"(,"nsec":)" << (timestamp_ns % kNanosecondsPerSecond) << '}';
}

void append_json_string_field(std::ostringstream& out, const char* key, const std::string& value) {
    out << R"(,")" << key << R"(":")" << json_escape(value) << '"';
}

void assign_payload(std::vector<std::byte>& payload, const std::string& json) {
    payload.reserve(json.size());
    std::transform(json.begin(), json.end(), std::back_inserter(payload), [](char character) {
        return static_cast<std::byte>(static_cast<unsigned char>(character));
    });
}

uint32_t compute_backlog_frames(uint32_t current_sequence, uint32_t latest_sequence) {
    if (latest_sequence < current_sequence) {
        return 0U;
    }

    return latest_sequence - current_sequence;
}

} // namespace

SharedDictLogger::SharedDictLogger(const std::string& config_path, std::string output_path) :
    _output_path(std::move(output_path)),
    _timestamp_mapper(SteadyClockUnixTimeMapper::from_current_clocks()) {
    _config.load(config_path);
    _initialize_streams();
    _open_writer();
    _register_channels();
}

void SharedDictLogger::_initialize_streams() {
    for (const auto& shm_cfg : _config.get_config().shared_memory) {
        _buffer_sizes[shm_cfg.name] = static_cast<uint32_t>(shm_cfg.num_frames);
    }

    for (const auto& cam_cfg : _config.get_config().cameras) {
        SensorStream stream;
        stream.initialized = false;
        stream.channel_id = 0;
        stream.height = cam_cfg.writer.height;
        stream.width = cam_cfg.writer.width;
        stream.name = cam_cfg.name;
        stream.reader = std::make_unique<SharedDictReader>(cam_cfg.name);
        stream.type = StreamType::CAMERA;
        stream.current_head = 0;
        stream.current_sequence = 0;
        stream.num_frames = std::max<uint32_t>(1, _buffer_sizes[cam_cfg.name]);
        stream.last_status_log = std::chrono::steady_clock::time_point::min();
        _sensor_streams.push_back(std::move(stream));
    }

    for (const auto& imu_cfg : _config.get_config().imus) {
        SensorStream stream;
        stream.initialized = false;
        stream.channel_id = 0;
        stream.height = 0;
        stream.width = 0;
        stream.name = imu_cfg.name;
        stream.reader = std::make_unique<SharedDictReader>(imu_cfg.name);
        stream.type = StreamType::IMU;
        stream.current_head = 0;
        stream.current_sequence = 0;
        stream.num_frames = std::max<uint32_t>(1, _buffer_sizes[imu_cfg.name]);
        stream.last_status_log = std::chrono::steady_clock::time_point::min();
        _sensor_streams.push_back(std::move(stream));
    }

    for (const auto& event_camera_cfg : _config.get_config().event_cameras) {
        SensorStream stream;
        stream.initialized = false;
        stream.channel_id = 0;
        stream.height = event_camera_cfg.height;
        stream.width = event_camera_cfg.width;
        stream.name = event_camera_cfg.name;
        stream.reader = std::make_unique<SharedDictReader>(event_camera_cfg.name);
        stream.type = StreamType::EVENT_CAMERA;
        stream.current_head = 0;
        stream.current_sequence = 0;
        stream.num_frames = std::max<uint32_t>(1, _buffer_sizes[event_camera_cfg.name]);
        stream.last_status_log = std::chrono::steady_clock::time_point::min();
        _sensor_streams.push_back(std::move(stream));
    }
}

void SharedDictLogger::_open_writer() {
    mcap::McapWriterOptions options("core");
    options.compression = mcap::Compression::Zstd;

    const auto status = _writer.open(_output_path, options);
    if (!status.ok()) {
        throw std::runtime_error("Failed to open mcap writer: " + std::string(status.message));
    }
}

void SharedDictLogger::_register_channels() {
    mcap::Metadata timestamp_metadata;
    timestamp_metadata.name = "core.timestamp";
    timestamp_metadata.metadata = {
        {"steady_to_unix_offset_ns", std::to_string(_timestamp_mapper.steady_to_unix_offset_ns())},
    };
    const auto metadata_status = _writer.write(timestamp_metadata);
    if (!metadata_status.ok()) {
        throw std::runtime_error("Failed to write timestamp metadata: " + std::string(metadata_status.message));
    }

    mcap::Schema image_schema(CompressedImageSchemaName, JsonSchemaEncoding, CompressedImageSchema.data());
    _writer.addSchema(image_schema);

    mcap::Schema imu_schema(ImuSchemaName, JsonSchemaEncoding, ImuSchema.data());
    _writer.addSchema(imu_schema);

    mcap::Schema event_array_schema(EventArraySchemaName, JsonSchemaEncoding, EventArraySchema.data());
    _writer.addSchema(event_array_schema);

    for (auto& stream : _sensor_streams) {
        if (stream.type == StreamType::CAMERA) {
            mcap::KeyValueMap const metadata{
            };
            mcap::Channel channel("/camera/" + stream.name + "/image/compressed", JsonMessageEncoding, image_schema.id, metadata);
            _writer.addChannel(channel);
            stream.channel_id = channel.id;

        } else if (stream.type == StreamType::IMU) {
            mcap::KeyValueMap const metadata{
            };
            mcap::Channel channel("/imu/" + stream.name, JsonMessageEncoding, imu_schema.id, metadata);
            _writer.addChannel(channel);
            stream.channel_id = channel.id;

        } else if (stream.type == StreamType::EVENT_CAMERA) {
            mcap::KeyValueMap const metadata{
            };
            mcap::Channel channel("/event_camera/" + stream.name, JsonMessageEncoding, event_array_schema.id, metadata);
            _writer.addChannel(channel);
            stream.channel_id = channel.id;

        } else {
            spdlog::warn("Unknown stream type for {}: {}", stream.name, static_cast<int>(stream.type));
        }
    }
}

SharedDictLogger::~SharedDictLogger() {
    std::lock_guard<std::mutex> const lock(_writer_mutex);
    _writer.close();
}

void SharedDictLogger::request_stop() {
    _stop.store(true, std::memory_order_release);
}

void SharedDictLogger::run() {
    spdlog::info("Logger started. Writing MCAP to {}", _output_path);
    std::vector<std::thread> workers;
    workers.reserve(_sensor_streams.size());

    for (auto& stream : _sensor_streams) {
        auto* stream_ptr = &stream;
        workers.emplace_back([this, stream_ptr]() {
            try {
                while (!_stop.load(std::memory_order_acquire)) {
                    const bool made_progress = _process_stream(*stream_ptr);
                    if (!made_progress) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    }
                    if (stream_ptr->type != StreamType::CAMERA && stream_ptr->type != StreamType::IMU
                        && stream_ptr->type != StreamType::EVENT_CAMERA) {
                        spdlog::warn("Unknown stream type for {}: {}", stream_ptr->name, static_cast<int>(stream_ptr->type));
                        break;
                    }
                }
            } catch (const std::exception& ex) {
                spdlog::error("Worker {} failed: {}", stream_ptr->name, ex.what());
                _stop.store(true, std::memory_order_release);
            }
        });
    }

    while (!_stop.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    for (auto& worker : workers) {
        if (worker.joinable()) {
            worker.join();
        }
    }

    {
        std::lock_guard<std::mutex> const lock(_writer_mutex);
        _writer.closeLastChunk();
    }
    spdlog::info("Logger stopping");
}

bool SharedDictLogger::_get_camera_payload(const DataEntry& entry, const SensorStream& stream, uint64_t timestamp_ns, std::vector<std::byte>& payload) {
    const size_t expected_size = stream.width * stream.height * 3;
    if (entry.data.size() < expected_size) {
        spdlog::warn("Camera {} sample too small: {} < {}", stream.name, entry.data.size(), expected_size);
        return false;
    }

    cv::Mat const rgb(
        static_cast<int>(stream.height),
        static_cast<int>(stream.width),
        CV_8UC3,
        const_cast<uint8_t*>(entry.data.data())
    );
    cv::Mat bgr;
    cv::cvtColor(rgb, bgr, cv::COLOR_RGB2BGR);
    std::vector<uint8_t> encoded;
    std::vector<int> const params{cv::IMWRITE_JPEG_QUALITY, _jpeg_quality};
    if (!cv::imencode(".jpg", bgr, encoded, params)) {
        spdlog::warn("Failed JPEG encoding for {}", stream.name);
        return false;
    }

    std::ostringstream json;
    json << '{';
    append_timestamp_json(json, timestamp_ns);
    append_json_string_field(json, "frame_id", stream.name);
    append_json_string_field(json, "data", base64_encode(encoded));
    append_json_string_field(json, "format", "jpeg");
    json << '}';
    assign_payload(payload, json.str());
    return true;
}

bool SharedDictLogger::_get_imu_payload(const DataEntry& entry, uint64_t timestamp_ns, std::vector<std::byte>& payload) {
    if (entry.data.size() < 3U * sizeof(float)) {
        spdlog::warn("IMU sample too small: {} < {}", entry.data.size(), 3U * sizeof(float));
        return false;
    }

    std::array<float, 3> xyz{};
    std::memcpy(xyz.data(), entry.data.data(), 3U * sizeof(float));
    std::ostringstream json;
    json << '{';
    append_timestamp_json(json, timestamp_ns);
    json << R"(,"sequence":)" << entry.sequence
         << R"(,"x":)" << xyz[0]
         << R"(,"y":)" << xyz[1]
         << R"(,"z":)" << xyz[2];
    json << '}';
    assign_payload(payload, json.str());
    return true;
}

bool SharedDictLogger::_get_event_camera_payload(const DataEntry& entry, uint64_t timestamp_ns, std::vector<std::byte>& payload) {
    // NOTE: This layout must match core/modules/event_camera/event_record.hpp
    // (EventBatchHeader + EventRecord). The constants are duplicated here rather than
    // included directly, since event_camera already depends on shared_memory and
    // including it back would create a module cycle.
    constexpr size_t kEventBatchHeaderSize = 8; // uint32_t num_events + uint32_t reserved
    constexpr size_t kEventRecordSize = 16;     // int64 timestamp_ns, uint16 x, uint16 y, uint8 polarity, uint8 reserved

    if (entry.data.size() < kEventBatchHeaderSize) {
        spdlog::warn("Event camera sample too small for header: {} < {}", entry.data.size(), kEventBatchHeaderSize);
        return false;
    }

    uint32_t num_events = 0;
    std::memcpy(&num_events, entry.data.data(), sizeof(num_events));

    const size_t records_size = static_cast<size_t>(num_events) * kEventRecordSize;
    if (entry.data.size() < kEventBatchHeaderSize + records_size) {
        spdlog::warn(
            "Event camera sample too small for {} events: {} < {}",
            num_events, entry.data.size(), kEventBatchHeaderSize + records_size
        );
        return false;
    }

    const std::vector<uint8_t> records(
        entry.data.begin() + static_cast<std::ptrdiff_t>(kEventBatchHeaderSize),
        entry.data.begin() + static_cast<std::ptrdiff_t>(kEventBatchHeaderSize + records_size)
    );

    std::ostringstream json;
    json << '{';
    append_timestamp_json(json, timestamp_ns);
    json << R"(,"sequence":)" << entry.sequence << R"(,"num_events":)" << num_events;
    append_json_string_field(json, "encoding", "core.EventRecord.v1");
    append_json_string_field(json, "data", base64_encode(records));
    json << '}';
    assign_payload(payload, json.str());
    return true;
}

bool SharedDictLogger::_write_entry(SensorStream& stream, const DataEntry& entry) {
    std::vector<std::byte> payload;
    const auto timestamp = _timestamp_mapper.to_unix_time_ns(entry.timestamp_ns);
    if (stream.type == StreamType::CAMERA) {
        if (!_get_camera_payload(entry, stream, timestamp, payload)) {
            return false;
        }

    } else if (stream.type == StreamType::IMU) {
        if (!_get_imu_payload(entry, timestamp, payload)) {
            return false;
        }

    } else if (stream.type == StreamType::EVENT_CAMERA) {
        if (!_get_event_camera_payload(entry, timestamp, payload)) {
            return false;
        }

    } else {
        spdlog::warn("Unknown stream type for {}: {}", stream.name, static_cast<int>(stream.type));
        return false;
    }

    mcap::Message msg;
    msg.channelId = stream.channel_id;
    msg.sequence = entry.sequence;
    msg.publishTime = static_cast<mcap::Timestamp>(timestamp);
    msg.logTime = static_cast<mcap::Timestamp>(timestamp);
    msg.data = payload.data();
    msg.dataSize = payload.size();

    std::lock_guard<std::mutex> const lock(_writer_mutex);
    const auto status = _writer.write(msg);
    if (!status.ok()) {
        spdlog::error("Failed to write sample for {}: {}", stream.name, status.message);
        return false;
    }

    return true;
}

void SharedDictLogger::_log_stream_status(SensorStream& stream, const DataEntry& latest_entry) {
    if (stream.last_status_log != std::chrono::steady_clock::time_point::min() &&
        (std::chrono::steady_clock::now() - stream.last_status_log) < kStatusLogInterval) {
        return;
    }

    const auto backlog_frames = compute_backlog_frames(stream.current_sequence, latest_entry.sequence);
    spdlog::info(
        "Logger status [{}]: logging seq {}, latest seq {}, backlog {} frame(s), logging head {}, data head {}, buffer size {}",
        stream.name,
        stream.current_sequence,
        latest_entry.sequence,
        backlog_frames,
        stream.current_head,
        latest_entry.head,
        stream.num_frames
    );

    stream.last_status_log = std::chrono::steady_clock::now();
}

bool SharedDictLogger::_process_stream(SensorStream& stream) {
    if (stream.reader == nullptr || !stream.reader->is_ready()) {
        return false;
    }

    bool wrote_any = false;
    for (;;) {
        // 0. Initialize the stream: This is where we start logging
        if (!stream.initialized) {
            DataEntry entry;

            stream.reader->read_latest(entry);

            if (entry.data.empty()) {
                spdlog::debug("Failed to read initial frame for {}, cannot initialize stream", stream.name);
                return wrote_any;
            }

            if (entry.head >= stream.num_frames) {
                spdlog::debug("Initial head {} for {} is out of bounds for num_frames {}, cannot initialize stream", entry.head, stream.name, stream.num_frames);
                return wrote_any;
            }

            stream.current_head = entry.head;
            stream.current_sequence = entry.sequence;
            stream.initialized = true;
            stream.last_status_log = std::chrono::steady_clock::now();
            spdlog::info("Initializing sensor stream {}: latest head is {}", stream.name, stream.current_head);
        }

        DataEntry latest_entry;
        stream.reader->read_latest(latest_entry);
        if (latest_entry.data.empty()) {
            return wrote_any;
        }

        _log_stream_status(stream, latest_entry);

        const auto progress = core::logger_detail::analyze_stream_progress(
            stream.current_sequence,
            latest_entry.sequence,
            stream.num_frames
        );
        if (progress.available_frames == 0U) {
            return wrote_any;
        }

        if (progress.needs_resync) {
            spdlog::warn(
                "Stream {} lagged by {} frame(s): expected sequence {}, latest sequence {}, buffer size {}. Resyncing to latest frame.",
                stream.name,
                progress.skipped_frames,
                stream.current_sequence,
                latest_entry.sequence,
                stream.num_frames
            );

            stream.current_head = latest_entry.head;
            stream.current_sequence = latest_entry.sequence;
            if (!_write_entry(stream, latest_entry)) {
                return wrote_any;
            }

            wrote_any = true;
            stream.current_head = (stream.current_head + 1) % stream.num_frames;
            stream.current_sequence = latest_entry.sequence + 1U;
            continue;
        }

        while (stream.current_sequence <= latest_entry.sequence) {
            DataEntry entry;
            stream.reader->read_absolute(entry, stream.current_head);
            if (entry.data.empty()) {
                return wrote_any;
            }

            if (entry.sequence < stream.current_sequence) {
                return wrote_any;
            }

            if (entry.sequence > stream.current_sequence) {
                const auto skipped_frames = entry.sequence - stream.current_sequence;
                spdlog::warn(
                    "Stream {} lost synchronization: expected sequence {}, got {}. Skipped {} frame(s) and resyncing to frame {}.",
                    stream.name,
                    stream.current_sequence,
                    entry.sequence,
                    skipped_frames,
                    entry.head
                );
                stream.current_head = entry.head;
                stream.current_sequence = entry.sequence;
            }

            if (!_write_entry(stream, entry)) {
                return wrote_any;
            }

            wrote_any = true;
            stream.current_head = (stream.current_head + 1) % stream.num_frames;
            stream.current_sequence = entry.sequence + 1U;
            if (stream.current_sequence > latest_entry.sequence) {
                break;
            }
        }
    }

    return wrote_any;
}

} // namespace core
