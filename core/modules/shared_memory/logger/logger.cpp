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
        CameraLogStream stream;
        stream.name = cam_cfg.name;
        stream.width = cam_cfg.writer.width;
        stream.height = cam_cfg.writer.height;
        stream.num_frames = std::max<uint32_t>(1, _buffer_sizes[cam_cfg.name]);
        stream.reader = std::make_unique<SharedDictReader>(cam_cfg.name);
        _camera_streams.push_back(std::move(stream));
    }

    for (const auto& imu_cfg : _config.get_config().imus) {
        IMULogStream stream;
        stream.name = imu_cfg.name;
        stream.num_frames = std::max<uint32_t>(1, _buffer_sizes[imu_cfg.name]);
        stream.reader = std::make_unique<SharedDictReader>(imu_cfg.name);
        _imu_streams.push_back(std::move(stream));
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

    for (auto& stream : _camera_streams) {
        mcap::Channel channel("/camera/" + stream.name + "/image/compressed", "binary", image_schema.id);
        _writer.addChannel(channel);
        stream.channel_id = channel.id;
    }

    for (auto& stream : _imu_streams) {
        mcap::Channel channel("/imu/" + stream.name, "binary", imu_schema.id);
        _writer.addChannel(channel);
        stream.channel_id = channel.id;
    }
}

SharedDictLogger::~SharedDictLogger() {
    _writer.close();
}

void SharedDictLogger::request_stop() {
    _stop.store(true, std::memory_order_release);
}

void SharedDictLogger::run() {
    spdlog::info("Logger started. Writing MCAP to {}", _output_path);
    while (!_stop.load(std::memory_order_acquire)) {
        for (auto& stream : _camera_streams) {
            _drain_camera_stream(stream);
        }
        for (auto& stream : _imu_streams) {
            _drain_imu_stream(stream);
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    _writer.closeLastChunk();
    spdlog::info("Logger stopping");
}


void SharedDictLogger::_drain_camera_stream(CameraLogStream& stream) {
    if (stream.reader == nullptr || !stream.reader->is_ready()) {
        return;
    }

    DataEntry latest;
    stream.reader->read_latest(latest);
    if (latest.data.empty()) {
        return;
    }

    if (!stream.initialized) {
        stream.last_sequence = latest.sequence - 1;
        stream.initialized = true;
    }

    const uint32_t delta = latest.sequence - stream.last_sequence;
    if (delta == 0) {
        return;
    }

    uint32_t to_drain = delta;
    if (to_drain > stream.num_frames) {
        spdlog::warn("Camera {} overflow: lost {} samples", stream.name, to_drain - stream.num_frames);
        to_drain = stream.num_frames;
    } else {
        spdlog::info("Draining {} samples from camera {}", to_drain, stream.name);
    }

    for (auto idx = static_cast<int32_t>(to_drain); idx >= 1; --idx) {
        DataEntry entry;
        stream.reader->read(entry, idx);
        if (entry.data.empty()) {
            continue;
        }

        if (stream.width == 0 || stream.height == 0) {
            continue;
        }

        const size_t expected_size = stream.width * stream.height * 3;
        if (entry.data.size() < expected_size) {
            spdlog::warn("Camera {} sample too small: {} < {}", stream.name, entry.data.size(), expected_size);
            continue;
        }

        cv::Mat rgb(
            static_cast<int>(stream.height),
            static_cast<int>(stream.width),
            CV_8UC3,
            entry.data.data()
        );

        std::vector<uint8_t> encoded;
        std::vector<int> params{cv::IMWRITE_JPEG_QUALITY, _jpeg_quality};
        if (!cv::imencode(".jpg", rgb, encoded, params)) {
            spdlog::warn("Failed JPEG encoding for {}", stream.name);
            continue;
        }

        std::vector<std::byte> payload;
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

        const auto timestamp = static_cast<mcap::Timestamp>(entry.timestamp_ns);
        mcap::Message msg;
        msg.channelId = stream.channel_id;
        msg.sequence = entry.sequence;
        msg.publishTime = timestamp;
        msg.logTime = timestamp;
        msg.data = payload.data();
        msg.dataSize = payload.size();

        const auto status = _writer.write(msg);
        if (!status.ok()) {
            spdlog::error("Failed to write camera sample for {}: {}", stream.name, status.message);
        }
    }

    stream.last_sequence = latest.sequence;
}

void SharedDictLogger::_drain_imu_stream(IMULogStream& stream) {
    if (stream.reader == nullptr || !stream.reader->is_ready()) {
        return;
    }

    DataEntry latest;
    stream.reader->read_latest(latest);
    if (latest.data.empty()) {
        return;
    }

    if (!stream.initialized) {
        stream.last_sequence = latest.sequence - 1;
        stream.initialized = true;
    }

    const uint32_t delta = latest.sequence - stream.last_sequence;
    if (delta == 0) {
        return;
    }

    uint32_t to_drain = delta;
    if (to_drain > stream.num_frames) {
        spdlog::warn("IMU {} overflow: lost {} samples", stream.name, to_drain - stream.num_frames);
        to_drain = stream.num_frames;
    }

    for (auto idx = static_cast<int32_t>(to_drain); idx >= 1; --idx) {
        DataEntry entry;
        stream.reader->read(entry, idx);
        if (entry.data.size() < 3 * sizeof(float)) {
            continue;
        }

        std::array<float, 3> xyz{};
        std::memcpy(xyz.data(), entry.data.data(), 3 * sizeof(float));

        std::vector<std::byte> payload;
        payload.reserve(sizeof(uint64_t) + sizeof(uint32_t) + 3 * sizeof(float));
        _append_value(payload, entry.timestamp_ns);
        _append_value(payload, entry.sequence);
        _append_value(payload, xyz[0]);
        _append_value(payload, xyz[1]);
        _append_value(payload, xyz[2]);

        const auto timestamp = static_cast<mcap::Timestamp>(entry.timestamp_ns);
        mcap::Message msg;
        msg.channelId = stream.channel_id;
        msg.sequence = entry.sequence;
        msg.publishTime = timestamp;
        msg.logTime = timestamp;
        msg.data = payload.data();
        msg.dataSize = payload.size();

        const auto status = _writer.write(msg);
        if (!status.ok()) {
            spdlog::error("Failed to write IMU sample for {}: {}", stream.name, status.message);
        }
    }

    stream.last_sequence = latest.sequence;
}

template <typename T>
void SharedDictLogger::_append_value(std::vector<std::byte>& out, const T& value) {
    const auto* ptr = reinterpret_cast<const std::byte*>(&value);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    out.insert(out.end(), ptr, ptr + sizeof(T));
}

} // namespace core
