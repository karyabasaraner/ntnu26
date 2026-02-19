#include "shared_dict.hpp"

#include <cstddef>
#include <cstdint>
#include <spdlog/spdlog.h>
#include <string>

namespace core {

void SharedDict::add(const std::string& key, const void* data, size_t length, uint64_t timestamp_ns) {
        // TODO(MJ): Implement shared memory storage logic here
        spdlog::info("Adding data to shared dict with key: {}, length: {}, timestamp: {}", key, length, timestamp_ns);
        data = data; // TODO(MJ): Store data in shared memory here
}

} // namespace core
