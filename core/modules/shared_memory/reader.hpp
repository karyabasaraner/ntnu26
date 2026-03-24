#ifndef WORKSPACES_CORE_CORE_MODULES_SHARED_MEMORY_SHARED_DICT_READER_HPP
#define WORKSPACES_CORE_CORE_MODULES_SHARED_MEMORY_SHARED_DICT_READER_HPP

#include "client.hpp"
#include "configs.hpp"
#include "ringbuffer.hpp"
#include "utils.hpp"

#include <cstdint>

namespace core {

class SharedDictReader : public SharedDictClient {
public:
    explicit SharedDictReader(std::string name);
    void read(DataEntry& entry, int32_t index_from_head);
    void read_latest(DataEntry& entry) { read(entry, 1); }

private:
    Buffer* _buffer{nullptr};
};

} // namespace core

#endif
