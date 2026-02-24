#ifndef WORKSPACES_CORE_CORE_MODULES_SHARED_MEMORY_SHARED_DICT_MASTER_HPP
#define WORKSPACES_CORE_CORE_MODULES_SHARED_MEMORY_SHARED_DICT_MASTER_HPP

#include <memory>
#include <string>
#include <unordered_map>

#include "../utils/configs.hpp"
#include "ringbuffer.hpp"


namespace core {

class SharedDictMaster {
public:
    explicit SharedDictMaster(const std::string& config_path);

private:
    Config _config;
    std::unordered_map<std::string, std::unique_ptr<RingBuffer>> _ringbuffer_map;

    void _initialize();
};

} // namespace core

#endif
