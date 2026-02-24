#ifndef WORKSPACES_CORE_CORE_MODULES_SHARED_MEMORY_SHARED_DICT_MASTER_HPP
#define WORKSPACES_CORE_CORE_MODULES_SHARED_MEMORY_SHARED_DICT_MASTER_HPP

#include <cstdint>
#include <string>

#include "../utils/configs.hpp"


namespace core {

class SharedDictMaster {
public:
    explicit SharedDictMaster(const std::string& config_path);
    ~SharedDictMaster();

    // Rule of five
    SharedDictMaster(const SharedDictMaster&) = delete;
    SharedDictMaster& operator=(const SharedDictMaster&) = delete;
    SharedDictMaster(SharedDictMaster&&) = delete;
    SharedDictMaster& operator=(SharedDictMaster&&) = delete;

private:
    Config _config;
    int _fd_shm{-1};
    uint32_t _size_per_buffer{0};
    uint32_t _total_size{0};
    uint8_t _num_ringbuffers{0};
    void* _map{nullptr};

    void _initialize_shm();
};

} // namespace core

#endif
