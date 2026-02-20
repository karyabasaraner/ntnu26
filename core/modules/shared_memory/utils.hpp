#ifndef DATA_ENTRY_HPP
#define DATA_ENTRY_HPP

#include <cstdint>
#include <string>
#include <vector>

namespace core {
struct DataEntry {
        std::string key;
        std::vector<uint8_t> data;
        uint64_t timestamp_ns;
};
} // namespace core

#endif
