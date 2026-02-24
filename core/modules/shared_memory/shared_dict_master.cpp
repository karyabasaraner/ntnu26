#include "shared_dict_master.hpp"

#include "../utils/configs.hpp"
#include "ringbuffer.hpp"

#include <memory>

namespace core {

SharedDictMaster::SharedDictMaster(const std::string& config_path) {
    _config.load(config_path);
    _initialize();
}

void SharedDictMaster::_initialize() {
    for (const auto& ringbuffer_config : _config.get_config().shared_memory.ringbuffers) {
        _ringbuffer_map[ringbuffer_config.name] = std::make_unique<RingBuffer>(ringbuffer_config);
    }
}

} // namespace core
