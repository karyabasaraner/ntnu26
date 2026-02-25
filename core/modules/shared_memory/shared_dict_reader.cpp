#include "shared_dict_reader.hpp"

#include "utils.hpp"

#include <spdlog/spdlog.h>
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

    auto computed_checksum = crc32(0, frame->data, static_cast<uInt>(_buffer->size_per_frame));
    if (computed_checksum != frame->checksum) {
        spdlog::error("Checksum mismatch for requested frame: computed {}, expected {}", computed_checksum, frame->checksum);
        return;
    }

    // TODO(MJ): How to make all of this ptr arithmetic more clean
    entry.key = std::string(_buffer->name);
    entry.data = std::vector<uint8_t>(frame->data, frame->data + _buffer->size_per_frame);
    entry.sequence = frame->sequence;
    entry.timestamp_ns = frame->timestamp_ns;

    spdlog::info("Read frame seq {}, ts {}", frame->sequence, frame->timestamp_ns);
}

} // namespace core