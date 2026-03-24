#include "reader.hpp"

#include "client.hpp"
#include "configs.hpp"
#include "ringbuffer.hpp"
#include "utils.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <spdlog/spdlog.h>
#include <utility>
#include <zconf.h>
#include <zlib.h>

namespace core {

SharedDictReader::SharedDictReader(std::string name) : SharedDictClient(std::move(name)), _buffer(get_buffer()) {
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

    const auto size = static_cast<std::size_t>(_buffer->size_per_frame);
    std::span<const uint8_t> const data{static_cast<const uint8_t*>(frame->data), size};

    auto computed_checksum = crc32(0, static_cast<const Bytef*>(data.data()), static_cast<uInt>(data.size()));
    if (computed_checksum != frame->checksum) {
        spdlog::error("Checksum mismatch for requested frame: computed {}, expected {}", computed_checksum, frame->checksum);
        return;
    }

    // TODO(MJ): How to make all of this ptr arithmetic more clean
    entry.data.assign(data.begin(), data.end());
    entry.head = last_frame_head;
    entry.key.assign(static_cast<const char*>(_buffer->name));
    entry.sequence = frame->sequence;
    entry.timestamp_ns = frame->timestamp_ns;

    // spdlog::debug("Read frame seq {}, ts {}", frame->sequence, frame->timestamp_ns);
}

} // namespace core
