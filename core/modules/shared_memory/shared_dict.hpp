#ifndef CORE_MODULES_MEMORY_SHARED_DICT_HPP
#define CORE_MODULES_MEMORY_SHARED_DICT_HPP

#include <cstdint>
#include <string>

namespace core {

class SharedDict {
public:
    SharedDict() = default;

    // Delete copy and move
    SharedDict(const SharedDict&) = delete;
    SharedDict& operator=(const SharedDict&) = delete;
    SharedDict(SharedDict&&) = delete;
    SharedDict& operator=(SharedDict&&) = delete;

    // TODO(MJ): Implement destruction here!
    ~SharedDict() = default;

    static void add(const std::string& key, const void* data, size_t length, uint64_t timestamp_ns);
};

} // namespace core

#endif
