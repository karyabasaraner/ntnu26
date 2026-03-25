#include "logger.hpp"
#include "schema.hpp"

#include <opencv2/core/mat.hpp>
#include <opencv2/imgcodecs.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace core {

SharedDictLogger::SharedDictLogger(const std::string& config_path, std::string output_path) : _output_path(std::move(output_path)) {
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
    mcap::Schema image_schema("core/CompressedImage", "schema", CompressedImageSchema.data());
    _writer.addSchema(image_schema);

    mcap::Schema imu_schema("core/ImuXYZ", "schema", ImuSchema.data());
    _writer.addSchema(imu_schema);

    for (auto& stream : _sensor_streams) {
        if (stream.type == StreamType::CAMERA) {
            mcap::Channel channel("/camera/" + stream.name + "/image/compressed", "binary", image_schema.id);
            _writer.addChannel(channel);
            stream.channel_id = channel.id;

        } else if (stream.type == StreamType::IMU) {
            mcap::Channel channel("/imu/" + stream.name, "binary", imu_schema.id);
            _writer.addChannel(channel);
            stream.channel_id = channel.id;

        } else {
            spdlog::warn("Unknown stream type for {}: {}", stream.name, static_cast<int>(stream.type));
        }
    }
}

SharedDictLogger::~SharedDictLogger() {
    std::lock_guard<std::mutex> lock(_writer_mutex);
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
                    if (stream_ptr->type == StreamType::CAMERA) {
                        _process_sensor_stream(*stream_ptr);
                        std::this_thread::sleep_for(std::chrono::milliseconds(16)); // ~60Hz is enough here

                    } else if (stream_ptr->type == StreamType::IMU) {
                        _process_sensor_stream(*stream_ptr);
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
        std::lock_guard<std::mutex> lock(_writer_mutex);
        _writer.closeLastChunk();
    }
    spdlog::info("Logger stopping");
}

void SharedDictLogger::_get_camera_payload(DataEntry& entry, const SensorStream& stream, std::vector<std::byte>& payload) {
    const size_t expected_size = stream.width * stream.height * 3;
    if (entry.data.size() < expected_size) {
        spdlog::warn("Camera {} sample too small: {} < {}", stream.name, entry.data.size(), expected_size);
        return;
    }

    cv::Mat rgb(static_cast<int>(stream.height), static_cast<int>(stream.width), CV_8UC3, entry.data.data());
    std::vector<uint8_t> encoded;
    std::vector<int> params{cv::IMWRITE_JPEG_QUALITY, _jpeg_quality};
    if (!cv::imencode(".jpg", rgb, encoded, params)) {
        spdlog::warn("Failed JPEG encoding for {}", stream.name);
        return;
    }

    payload.reserve(sizeof(uint64_t) + 4 * sizeof(uint32_t) + 2 * sizeof(uint8_t) + encoded.size());

    _append_value(payload, entry.timestamp_ns);
    _append_value(payload, entry.sequence);

    const auto width = static_cast<uint32_t>(stream.width);
    const auto height = static_cast<uint32_t>(stream.height);
    const auto channels = static_cast<uint8_t>(3);
    const auto quality = static_cast<uint8_t>(_jpeg_quality);
    const auto jpeg_size = static_cast<uint32_t>(encoded.size());

    _append_value(payload, width);
    _append_value(payload, height);
    _append_value(payload, channels);
    _append_value(payload, quality);
    _append_value(payload, jpeg_size);

    payload.insert(
        payload.end(),
        reinterpret_cast<const std::byte*>(encoded.data()),
        reinterpret_cast<const std::byte*>(encoded.data() + encoded.size())
    );
}

void SharedDictLogger::_get_imu_payload(const DataEntry& entry, std::vector<std::byte>& payload) {
    std::array<float, 3> xyz{};
    std::memcpy(xyz.data(), entry.data.data(), 3 * sizeof(float));

    payload.reserve(sizeof(uint64_t) + sizeof(uint32_t) + 3 * sizeof(float));
    _append_value(payload, entry.timestamp_ns);
    _append_value(payload, entry.sequence);
    _append_value(payload, xyz[0]);
    _append_value(payload, xyz[1]);
    _append_value(payload, xyz[2]);

}


void SharedDictLogger::_process_sensor_stream(SensorStream& stream) {
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
    if (stream.type == StreamType::CAMERA) {
        _get_camera_payload(entry, stream, payload);

    } else if (stream.type == StreamType::IMU) {
        _get_imu_payload(entry, payload);

    } else {
        spdlog::warn("Unknown stream type for {}: {}", stream.name, static_cast<int>(stream.type));
        return;
    }

    const auto timestamp = static_cast<mcap::Timestamp>(entry.timestamp_ns);
    mcap::Message msg;
    msg.channelId = stream.channel_id;
    msg.sequence = entry.sequence;
    msg.publishTime = timestamp;
    msg.logTime = timestamp;
    msg.data = payload.data();
    msg.dataSize = payload.size();

    std::lock_guard<std::mutex> lock(_writer_mutex);
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

template <typename T>
void SharedDictLogger::_append_value(std::vector<std::byte>& out, const T& value) {
    const auto* ptr = reinterpret_cast<const std::byte*>(&value);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    out.insert(out.end(), ptr, ptr + sizeof(T));
}

} // namespace core
