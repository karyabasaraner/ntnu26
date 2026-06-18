#ifndef WORKSPACES_CORE_CORE_MODULES_SHARED_MEMORY_UTILS_HPP
#define WORKSPACES_CORE_CORE_MODULES_SHARED_MEMORY_UTILS_HPP

#include <cstdint>
#include <string>
#include <vector>

namespace core {

enum class TimestampSource : uint8_t {
        HOST_FALLBACK = 0,
        V4L2_BUFFER = 1,
        IIO_HARDWARE = 2,
};

enum class TimestampClockDomain : uint8_t {
        UNKNOWN = 0,
        MONOTONIC = 1,
        UNIX_EPOCH = 2,
};

enum class TimestampQuality : uint8_t {
        FALLBACK = 0,
        KERNEL = 1,
        HARDWARE = 2,
};

struct TimestampMetadata {
        uint64_t host_receive_timestamp_ns{0};
        TimestampSource source{TimestampSource::HOST_FALLBACK};
        TimestampClockDomain clock_domain{TimestampClockDomain::MONOTONIC};
        TimestampQuality quality{TimestampQuality::FALLBACK};
};

inline std::string timestamp_source_to_string(TimestampSource source) {
        switch (source) {
                case TimestampSource::HOST_FALLBACK:
                        return "host_fallback";
                case TimestampSource::V4L2_BUFFER:
                        return "v4l2_buffer";
                case TimestampSource::IIO_HARDWARE:
                        return "iio_hardware";
        }
        return "unknown";
}

inline std::string timestamp_clock_domain_to_string(TimestampClockDomain domain) {
        switch (domain) {
                case TimestampClockDomain::UNKNOWN:
                        return "unknown";
                case TimestampClockDomain::MONOTONIC:
                        return "monotonic";
                case TimestampClockDomain::UNIX_EPOCH:
                        return "unix_epoch";
        }
        return "unknown";
}

inline std::string timestamp_quality_to_string(TimestampQuality quality) {
        switch (quality) {
                case TimestampQuality::FALLBACK:
                        return "fallback";
                case TimestampQuality::KERNEL:
                        return "kernel";
                case TimestampQuality::HARDWARE:
                        return "hardware";
        }
        return "unknown";
}

struct DataEntry {
        std::string key;
        std::vector<uint8_t> data;
        uint32_t head;
        uint32_t sequence;
        uint64_t timestamp_ns;
        TimestampMetadata timestamp_metadata;
};

static const std::string SHM_NAME = "/shared_dict";
} // namespace core

#endif
