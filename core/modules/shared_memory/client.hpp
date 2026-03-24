#ifndef WORKSPACES_CORE_CORE_MODULES_SHARED_MEMORY_SHARED_DICT_CLIENT_HPP
#define WORKSPACES_CORE_CORE_MODULES_SHARED_MEMORY_SHARED_DICT_CLIENT_HPP

#include "../utils/configs.hpp"
#include "ringbuffer.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>

namespace core {

class SharedDictClient {
public:
    explicit SharedDictClient(std::string name);
    ~SharedDictClient();

    SharedDictClient(const SharedDictClient&) = delete;
    SharedDictClient(SharedDictClient&&) = delete;
    SharedDictClient& operator=(const SharedDictClient&) = delete;
    SharedDictClient& operator=(SharedDictClient&&) = delete;

    size_t get_shm_total_size() const { return _shm_total_size; }
    void* get_shm_map() const { return _map; }

    bool is_ready() const;
    Buffer* get_buffer() const;
    DataFrame* get_frame_by_index(uint32_t head_index);
    uint32_t get_head(int32_t index_from_head) const;

private:
    Buffer* _buffer{nullptr};
    std::string _name;
    int _fd_shm{-1};
    size_t _shm_total_size{0};
    void* _map{nullptr};
    std::unordered_map<std::string, Buffer*> _buffer_map;

    void _get_shm_map() ;
    void _get_shm_structure();
    void _get_transforms_from_config();
    void _process_queue();
};

} // namespace core

#endif
