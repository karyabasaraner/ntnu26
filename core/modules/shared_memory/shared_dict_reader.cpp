#include "shared_dict_reader.hpp"

#include "utils.hpp"

#include <spdlog/spdlog.h>
#include <span>
#include <zlib.h>

namespace core {

SharedDictReader::SharedDictReader(CameraConfig config) : SharedDictClient(std::move(config)), _buffer(get_buffer()) {
}

void SharedDictReader::read(DataEntry& entry, uint32_t index_from_head) {
    if (!is_ready()) {
        spdlog::warn("SharedDictReader is not ready, cannot read");
        return;
    }

    ImageFrame* frame = get_requested_frame(index_from_head);
    if (frame == nullptr) {
        spdlog::warn("Failed to get requested frame, cannot read");
        return;
    }

    const auto size = static_cast<std::size_t>(_buffer->size_per_frame);
    std::span<const uint8_t> data{static_cast<const uint8_t*>(frame->data), size};

    auto computed_checksum = crc32(0, static_cast<const Bytef*>(data.data()), static_cast<uInt>(data.size()));
    if (computed_checksum != frame->checksum) {
        spdlog::error("Checksum mismatch for requested frame: computed {}, expected {}", computed_checksum, frame->checksum);
        return;
    }

    // TODO(MJ): How to make all of this ptr arithmetic more clean
    entry.key.assign(static_cast<const char*>(_buffer->name));
    entry.data.assign(data.begin(), data.end());
    entry.sequence = frame->sequence;
    entry.timestamp_ns = frame->timestamp_ns;

    spdlog::info("Read frame seq {}, ts {}", frame->sequence, frame->timestamp_ns);
}

} // namespace core