#ifndef WORKSPACES_CORE_CORE_MODULES_SHARED_MEMORY_UTILS_HPP
#define WORKSPACES_CORE_CORE_MODULES_SHARED_MEMORY_UTILS_HPP

#include <cstdint>
#include <string>
#include <vector>

namespace core {
struct DataEntry {
        std::string key;
        std::vector<uint8_t> data;
        uint32_t sequence;
        uint64_t timestamp_ns;
};

static const std::string SHM_NAME = "/shared_dict";
} // namespace core

#endif
