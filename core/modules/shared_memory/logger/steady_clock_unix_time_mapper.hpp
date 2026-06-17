#ifndef WORKSPACES_CORE_CORE_MODULES_SHARED_MEMORY_LOGGER_STEADY_CLOCK_UNIX_TIME_MAPPER_HPP
#define WORKSPACES_CORE_CORE_MODULES_SHARED_MEMORY_LOGGER_STEADY_CLOCK_UNIX_TIME_MAPPER_HPP

#include <cstdint>

namespace core {

class SteadyClockUnixTimeMapper {
public:
    explicit SteadyClockUnixTimeMapper(int64_t steady_to_unix_offset_ns);

    [[nodiscard]] static SteadyClockUnixTimeMapper from_current_clocks();
    [[nodiscard]] uint64_t to_unix_time_ns(uint64_t steady_timestamp_ns) const;
    [[nodiscard]] int64_t steady_to_unix_offset_ns() const;

private:
    int64_t _steady_to_unix_offset_ns{0};
};

} // namespace core

#endif // WORKSPACES_CORE_CORE_MODULES_SHARED_MEMORY_LOGGER_STEADY_CLOCK_UNIX_TIME_MAPPER_HPP
