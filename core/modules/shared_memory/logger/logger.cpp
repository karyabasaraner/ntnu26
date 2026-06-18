#include "logger.hpp"
#include "../client/reader.hpp"
#include "../utils.hpp"
#include "mcap/types.hpp"
#include "mcap/writer.hpp"
#include "schema.hpp"
#include "steady_clock_unix_time_mapper.hpp"

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

void assign_payload(std::vector<std::byte>& payload, const std::string& json) {
    payload.reserve(json.size());
    std::transform(json.begin(), json.end(), std::back_inserter(payload), [](char character) {
        return static_cast<std::byte>(static_cast<unsigned char>(character));
    });
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
        {"timestamp_domain", "unix_epoch"},
        {"source_timestamp_domain", "steady_clock"},
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

    for (auto& stream : _sensor_streams) {
        if (stream.type == StreamType::CAMERA) {
            mcap::Channel channel("/camera/" + stream.name + "/image/compressed", JsonMessageEncoding, image_schema.id);
            _writer.addChannel(channel);
            stream.channel_id = channel.id;

        } else if (stream.type == StreamType::IMU) {
            mcap::Channel channel("/imu/" + stream.name, JsonMessageEncoding, imu_schema.id);
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
                    _process_stream(*stream_ptr);
                    if (stream_ptr->type == StreamType::CAMERA) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(16)); // ~60Hz is enough here
                    } else if (stream_ptr->type == StreamType::IMU) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(1)); // ~1000Hz
                    } else {
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

bool SharedDictLogger::_get_camera_payload(DataEntry& entry, const SensorStream& stream, uint64_t timestamp_ns, std::vector<std::byte>& payload) {
    const size_t expected_size = stream.width * stream.height * 3;
    if (entry.data.size() < expected_size) {
        spdlog::warn("Camera {} sample too small: {} < {}", stream.name, entry.data.size(), expected_size);
        return false;
    }

    cv::Mat const rgb(static_cast<int>(stream.height), static_cast<int>(stream.width), CV_8UC3, entry.data.data());
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
    json << R"(,"frame_id":")" << json_escape(stream.name)
         << R"(,"data":")" << base64_encode(encoded)
         << R"(,"format":"jpeg"})";
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
         << R"(,"z":)" << xyz[2]
         << '}';
    assign_payload(payload, json.str());
    return true;
}

void SharedDictLogger::_process_stream(SensorStream& stream) {
    if (stream.reader == nullptr || !stream.reader->is_ready()) {
        return;
    }

    // 0. Initialize the stream: This is where we start logging
    if (!stream.initialized) {
        DataEntry entry;

        // TODO(MJ): latest, zero or oldest?
        stream.reader->read_latest(entry);

        if (entry.data.empty()) {
            spdlog::debug("Failed to read initial frame for {}, cannot initialize stream", stream.name);
            return;
        }

        if (entry.head >= stream.num_frames) {
            spdlog::debug("Initial head {} for {} is out of bounds for num_frames {}, cannot initialize stream", entry.head, stream.name, stream.num_frames);
            return;
        }

        stream.current_head = entry.head;
        stream.current_sequence = entry.sequence;
        stream.initialized = true;
        spdlog::info("Initializing sensor stream {}: latest head is {}", stream.name, stream.current_head);
    }

    // 1. We want to write the current_head to the log file
    // spdlog::info("Reading absolute index {}", stream.current_head);
    DataEntry entry;
    stream.reader->read_absolute(entry, stream.current_head);
    if (entry.data.empty()) {
        return;
    }

    // Option 1: Read sequence is behind: probably old data that will be overwritten soon, skip it
    if (entry.sequence < stream.current_sequence) {
        return;
    }

    if (entry.sequence > stream.current_sequence) {
        spdlog::warn("Sequence jump for {}: expected {}, got {}", stream.name, stream.current_sequence, entry.sequence);
        stream.current_sequence = entry.sequence;
    }

    // spdlog::info("Entry sequence {}, stream current_sequence {}", entry.sequence, stream.current_sequence);

    // 2. Write the data
    std::vector<std::byte> payload;
    const auto timestamp = _timestamp_mapper.to_unix_time_ns(entry.timestamp_ns);
    if (stream.type == StreamType::CAMERA) {
        if (!_get_camera_payload(entry, stream, timestamp, payload)) {
            return;
        }

    } else if (stream.type == StreamType::IMU) {
        if (!_get_imu_payload(entry, timestamp, payload)) {
            return;
        }

    } else {
        spdlog::warn("Unknown stream type for {}: {}", stream.name, static_cast<int>(stream.type));
        return;
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
        spdlog::error("Failed to write camera sample for {}: {}", stream.name, status.message);
        return;
    }

    // 3. Advance current_head, and wrap around if needed
    stream.current_head = (stream.current_head + 1) % stream.num_frames;
    stream.current_sequence = entry.sequence + 1;
    // spdlog::info("Logged {} sample: head {}, sequence {}", stream.name, stream.current_head, stream.current_sequence);

    // 4. Log warnings
    if (stream.current_head == stream.num_frames - 1) {
        const auto head_distance = (stream.current_head - entry.head + stream.num_frames) % stream.num_frames;
        if (head_distance > stream.num_frames / 2) {
            spdlog::warn("Stream {}: head distance {}", stream.name, head_distance);
        } else {
            spdlog::info("Stream {} ok: head distance {}", stream.name, head_distance);
        }
    }
}

} // namespace core
