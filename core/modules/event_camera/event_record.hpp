#ifndef WORKSPACES_CORE_CORE_MODULES_EVENT_CAMERA_EVENT_RECORD_HPP
#define WORKSPACES_CORE_CORE_MODULES_EVENT_CAMERA_EVENT_RECORD_HPP

#include <cstdint>

namespace core {

// One raw event ("brightness change") as delivered by an event-based sensor.
// Deliberately SDK-agnostic: backends (e.g. Metavision/Prophesee) convert their own
// event types into this before publishing, so the shared-memory/MCAP path never
// depends on a specific vendor SDK's in-memory layout.
struct EventRecord {
    int64_t timestamp_ns{0}; // absolute capture time of this single event
    uint16_t x{0};
    uint16_t y{0};
    uint8_t polarity{0}; // 0 = OFF (brightness decrease), 1 = ON (brightness increase)
    uint8_t reserved{0};
};

static_assert(sizeof(EventRecord) == 16, "EventRecord layout is serialized as-is into shared memory and MCAP");

// Header prefixing a batch of EventRecord entries within one published shared-memory
// frame: num_events records of sizeof(EventRecord) bytes each immediately follow.
struct EventBatchHeader {
    uint32_t num_events{0};
    uint32_t reserved{0};
};

static_assert(sizeof(EventBatchHeader) == 8, "EventBatchHeader layout is serialized as-is into shared memory and MCAP");

} // namespace core

#endif
