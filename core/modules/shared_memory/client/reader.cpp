#include "reader.hpp"

#include "../ringbuffer.hpp"
#include "../utils.hpp"
#include "client.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <spdlog/spdlog.h>
#include <string>
#include <utility>
#include <zconf.h>
#include <zlib.h>

namespace core {

SharedDictReader::SharedDictReader(std::string name) : SharedDictClient(std::move(name)), _buffer(get_buffer()) {
}

void SharedDictReader::_populate_data_enty(DataEntry& entry, DataFrame* frame, uint32_t head_index) {
    const auto size = static_cast<std::size_t>(_buffer->size_per_frame);
    std::span<const uint8_t> const data{static_cast<const uint8_t*>(frame->data), size};

    auto computed_checksum = crc32(0, static_cast<const Bytef*>(data.data()), static_cast<uInt>(data.size()));
    if (computed_checksum != frame->checksum) {
        // spdlog::error("Checksum mismatch for requested frame: computed {}, expected {}", computed_checksum, frame->checksum);
        return;
    }

    // TODO(MJ): How to make all of this ptr arithmetic more clean
    entry.data.assign(data.begin(), data.end());
    entry.key.assign(static_cast<const char*>(_buffer->name));
    entry.head = head_index;
    entry.sequence = frame->sequence;
    entry.timestamp_ns = frame->timestamp_ns;
}

void SharedDictReader::read(DataEntry& entry, int32_t index_from_head) {
    if (!is_ready()) {
        spdlog::warn("SharedDictReader is not ready, cannot read");
        return;
    }

    const auto last_frame_head = get_head(index_from_head);
    spdlog::debug("Requested frame {} with index_from_head={}", last_frame_head, index_from_head);
    DataFrame* frame = get_frame_by_index(last_frame_head);
    if (frame == nullptr) {
        spdlog::warn("Failed to get requested frame, cannot read");
        return;
    }
    _populate_data_enty(entry, frame, last_frame_head);
}

void SharedDictReader::read_absolute(DataEntry& entry, uint32_t absolute_index) {
    if (!is_ready()) {
        spdlog::warn("SharedDictReader is not ready, cannot read by absolute index");
        return;
    }

    if (absolute_index >= _buffer->num_frames) {
        spdlog::warn("Requested absolute index {} is out of bounds for buffer with num_frames {}", absolute_index, _buffer->num_frames);
        return;
    }

    // spdlog::info("Reading abs idx {}, head {}", absolute_index, get_head(0));
    DataFrame* frame = get_frame_by_index(absolute_index);
    if (frame == nullptr) {
        spdlog::warn("Failed to get frame by absolute index {}, cannot read", absolute_index);
        return;
    }
    _populate_data_enty(entry, frame, absolute_index);
}

void SharedDictReader::read_oldest(DataEntry& entry) {
    if (!is_ready()) {
        spdlog::warn("SharedDictReader is not ready, cannot read oldest");
        return;
    }

    // Get frame ahead of the current head
    uint32_t index = get_head(-1);
    DataFrame* frame = get_frame_by_index(index);

    // Check if zero frame is valid
    if (frame == nullptr) {
        index = 0;
        frame = get_frame_by_index(index);

        if (frame == nullptr) {
            spdlog::warn("Failed to get oldest, both with oldest and zero frame");
            return;
        }
    }
    _populate_data_enty(entry, frame, index);
}

} // namespace core
