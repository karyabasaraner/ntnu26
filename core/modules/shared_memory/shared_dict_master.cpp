#include "shared_dict_master.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <spdlog/spdlog.h>
#include <stdexcept>
#include <string>
#include <sys/mman.h>
#include <unistd.h>

#include "../utils/configs.hpp"
#include "ringbuffer.hpp"

namespace core {

SharedDictMaster::SharedDictMaster(const std::string& config_path) {
    _config.load(config_path);
    _initialize_shm();
}

SharedDictMaster::~SharedDictMaster() {
    if (_map != nullptr) {
        munmap(_map, _total_size);
    }

    if (_fd_shm >= 0) {
        ::close(_fd_shm);
        shm_unlink("/shared_dict");
    }
    spdlog::info("SharedDictMaster resources cleaned up");
}

void SharedDictMaster::_initialize_shm() {
    auto rb_config = _config.get_config().shared_memory;
    _num_ringbuffers = rb_config.size();

    // Find the total size of the shared memory segment
    _total_size = static_cast<uint32_t>(offsetof(Layout, buffers));
    for (const auto& config : rb_config) {
        // NOTE: offsetof computes the distance of a specific data member from the beginning.
        _size_per_buffer = static_cast<uint32_t>(offsetof(Buffer, frames));
        _size_per_buffer += config.num_frames * static_cast<uint32_t>(sizeof(ImageFrame) + config.size_per_frame);
        _total_size += _size_per_buffer;
        spdlog::info("size_per_buffer {}", _size_per_buffer);
    }

    _fd_shm = shm_open("/shared_dict", O_CREAT | O_RDWR, 0666);
    if (_fd_shm < 0) {
        spdlog::error("Failed to create shared memory segment");
        throw std::runtime_error("Failed to create shared memory segment");
    }

    if (ftruncate(_fd_shm, _total_size) < 0) {
        spdlog::error("Failed to set size of shared memory segment");
        throw std::runtime_error("Failed to set size of shared memory segment");
    }

    _map = mmap(nullptr, _total_size, PROT_READ | PROT_WRITE, MAP_SHARED, _fd_shm, 0);
    if (_map == MAP_FAILED) {
        spdlog::error("mmap failed");
        ::close(_fd_shm);
        throw std::runtime_error("mmap failed");
    }
    std::memset(_map, 0, _total_size);
    spdlog::info("Initialized shared memory with {} bytes", _total_size);
}

} // namespace core
