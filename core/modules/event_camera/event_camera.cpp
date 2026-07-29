#include "event_camera.hpp"

#include "configs.hpp"
#include "event_record.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <system_error>
#include <utility>
#include <vector>

#include <spdlog/spdlog.h>

namespace core {

EventCamera::EventCamera(EventCameraConfig config) :
    _config(std::move(config)),
    // Event batches are raw binary blobs (EventBatchHeader + EventRecord[]), not
    // images, so no writer transform chain applies here.
    _shdict_writer(_config.name, WriterConfig{}) {}

bool EventCamera::start() {
    std::lock_guard<std::mutex> const lock(_state_mutex);
    if (_state != State::initialized || !is_valid()) {
        return false;
    }

    if (!_start_acquisition()) {
        return false;
    }

    _running = true;
    try {
        _worker = std::thread([this] {
            _capture_loop();
            _running = false;
        });
    } catch (const std::system_error& error) {
        _running = false;
        _stop_acquisition();
        spdlog::error("Failed to start capture thread for event camera {}: {}", _config.name, error.what());
        return false;
    }

    _state = State::running;
    spdlog::info("Started streaming for event camera: {}", _config.name);
    return true;
}

void EventCamera::stop() noexcept {
    std::lock_guard<std::mutex> const lock(_state_mutex);
    if (_state == State::stopped) {
        return;
    }

    spdlog::info("Stopping event camera: {}", _config.name);
    _running = false;

    if (_state == State::running && !_stop_acquisition()) {
        spdlog::warn("Failed to stop acquisition for event camera: {}", _config.name);
    }

    if (_worker.joinable()) {
        _worker.join();
    }
    spdlog::info("Stopped capture thread for event camera: {}", _config.name);

    _post_stop();
    _state = State::stopped;
}

void EventCamera::process_events(const EventRecord* events, size_t count) {
    if (events == nullptr || count == 0) {
        return;
    }

    const size_t chunk_capacity = _config.max_events_per_frame > 0
        ? static_cast<size_t>(_config.max_events_per_frame)
        : count;
    // Every publish must be exactly the buffer's fixed size_per_frame: the shared
    // memory reader always checksums the whole slot, so a shorter, un-padded write
    // would just look corrupt and get silently dropped on read. Slots below capacity
    // (the last chunk of a batch) are zero-padded past num_events instead.
    const size_t frame_size = sizeof(EventBatchHeader) + (chunk_capacity * sizeof(EventRecord));

    for (size_t offset = 0; offset < count; offset += chunk_capacity) {
        const size_t chunk_count = std::min(chunk_capacity, count - offset);

        std::vector<uint8_t> payload(frame_size, 0U);
        EventBatchHeader header{};
        header.num_events = static_cast<uint32_t>(chunk_count);
        std::memcpy(payload.data(), &header, sizeof(EventBatchHeader));
        std::memcpy(payload.data() + sizeof(EventBatchHeader), events + offset, chunk_count * sizeof(EventRecord));

        const uint64_t chunk_timestamp_ns = static_cast<uint64_t>(events[offset].timestamp_ns);
        _shdict_writer.add(_config.name, payload.data(), payload.size(), _sequence++, chunk_timestamp_ns);
    }
}

} // namespace core
