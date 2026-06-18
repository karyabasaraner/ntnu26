#ifndef WORKSPACES_CORE_CORE_MODULES_SHARED_MEMORY_LOGGER_STREAM_PROGRESS_HPP
#define WORKSPACES_CORE_CORE_MODULES_SHARED_MEMORY_LOGGER_STREAM_PROGRESS_HPP

#include <cstdint>

namespace core {
namespace logger_detail {

struct StreamProgress {
    uint32_t available_frames{0};
    bool needs_resync{false};
    uint32_t skipped_frames{0};
};

[[nodiscard]] constexpr StreamProgress analyze_stream_progress(
    uint32_t current_sequence,
    uint32_t latest_sequence,
    uint32_t buffer_size
) {
    if (buffer_size == 0U || latest_sequence < current_sequence) {
        return {};
    }

    const auto available_frames = latest_sequence - current_sequence + 1U;
    const auto needs_resync = available_frames > buffer_size;
    return StreamProgress{
        .available_frames = available_frames,
        .needs_resync = needs_resync,
        .skipped_frames = needs_resync ? available_frames - buffer_size : 0U,
    };
}

} // namespace logger_detail
} // namespace core

#endif // WORKSPACES_CORE_CORE_MODULES_SHARED_MEMORY_LOGGER_STREAM_PROGRESS_HPP
