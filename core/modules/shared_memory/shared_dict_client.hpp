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
    explicit SharedDictClient(CameraConfig config={});
    ~SharedDictClient();

    SharedDictClient(const SharedDictClient&) = delete;
    SharedDictClient(SharedDictClient&&) = delete;
    SharedDictClient& operator=(const SharedDictClient&) = delete;
    SharedDictClient& operator=(SharedDictClient&&) = delete;

    size_t get_shm_total_size() const { return _shm_total_size; }
    void* get_shm_map() const { return _map; }

    bool is_ready() const;
    Buffer* get_buffer() const;
    CameraConfig get_config() const { return _config; }
    ImageFrame* get_requested_frame(uint32_t index_from_head=0);

private:
    Buffer* _buffer{nullptr};
    CameraConfig _config;
    int _fd_shm{-1};
    size_t _shm_total_size{0};
    void* _map{nullptr};
    std::unordered_map<std::string, Buffer*> _buffer_map;

    uint32_t _get_requested_head(uint32_t index_from_head=0) const;
    void _get_shm_map() ;
    void _get_shm_structure();
    void _get_transforms_from_config();
    void _process_queue();
};

} // namespace core

#endif
