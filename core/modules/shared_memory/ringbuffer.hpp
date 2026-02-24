#ifndef WORKSPACES_CORE_CORE_MODULES_SHARED_MEMORY_RINGBUFFER_HPP
#define WORKSPACES_CORE_CORE_MODULES_SHARED_MEMORY_RINGBUFFER_HPP

#include "../utils/configs.hpp"

namespace core {

class RingBuffer {
public:
    explicit RingBuffer(const RingBufferConfig& config);
};

} // namespace core

#endif