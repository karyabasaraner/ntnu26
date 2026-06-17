#include "steady_clock_unix_time_mapper.hpp"

#include <chrono>
#include <cstdint>
#include <limits>
#include <stdexcept>

namespace core {

SteadyClockUnixTimeMapper::SteadyClockUnixTimeMapper(int64_t steady_to_unix_offset_ns) :
    _steady_to_unix_offset_ns(steady_to_unix_offset_ns) {}

SteadyClockUnixTimeMapper SteadyClockUnixTimeMapper::from_current_clocks() {
    const auto steady_before = std::chrono::steady_clock::now();
    const auto system_now = std::chrono::system_clock::now();
    const auto steady_after = std::chrono::steady_clock::now();
    const auto steady_midpoint = steady_before + ((steady_after - steady_before) / 2);

    const auto steady_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(steady_midpoint.time_since_epoch()).count();
    const auto system_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(system_now.time_since_epoch()).count();
    return SteadyClockUnixTimeMapper(system_ns - steady_ns);
}

uint64_t SteadyClockUnixTimeMapper::to_unix_time_ns(uint64_t steady_timestamp_ns) const {
    if (steady_timestamp_ns > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
        throw std::runtime_error("steady timestamp is too large to convert to Unix time");
    }

    const auto steady_timestamp = static_cast<int64_t>(steady_timestamp_ns);
    if (_steady_to_unix_offset_ns > 0 && steady_timestamp > std::numeric_limits<int64_t>::max() - _steady_to_unix_offset_ns) {
        throw std::runtime_error("steady timestamp conversion to Unix time overflows");
    }
    if (_steady_to_unix_offset_ns < 0 && steady_timestamp < std::numeric_limits<int64_t>::min() - _steady_to_unix_offset_ns) {
        throw std::runtime_error("steady timestamp conversion to Unix time underflows");
    }

    const auto unix_timestamp = steady_timestamp + _steady_to_unix_offset_ns;
    if (unix_timestamp < 0) {
        throw std::runtime_error("steady timestamp conversion produced a negative Unix timestamp");
    }
    return static_cast<uint64_t>(unix_timestamp);
}

int64_t SteadyClockUnixTimeMapper::steady_to_unix_offset_ns() const {
    return _steady_to_unix_offset_ns;
}

} // namespace core
