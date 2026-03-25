#ifndef WORKSPACES_CORE_CORE_MODULES_SHARED_MEMORY_SHARED_DICT_READER_HPP
#define WORKSPACES_CORE_CORE_MODULES_SHARED_MEMORY_SHARED_DICT_READER_HPP

#include "../ringbuffer.hpp"
#include "../utils.hpp"
#include "client.hpp"

#include <cstdint>
#include <string>

namespace core {

class SharedDictReader : public SharedDictClient {
public:
    explicit SharedDictReader(std::string name);
    void read_absolute(DataEntry& entry, uint32_t absolute_index);
    void read_latest(DataEntry& entry) { read(entry, 1); }
    void read(DataEntry& entry, int32_t index_from_head);

private:
    Buffer* _buffer{nullptr};
    void _populate_data_enty(DataEntry& entry, DataFrame* frame, uint32_t head_index);
};

} // namespace core

#endif
