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
#include "utils.hpp"

namespace core {

SharedDictMaster::SharedDictMaster(const std::string& config_path) : _initialized(false) {
    _config.load(config_path);
    _initialize_shm();
    _initialize_metadata();
}

SharedDictMaster::~SharedDictMaster() {
    if (_map != nullptr) {
        munmap(_map, _total_size);
    }

    if (_fd_shm >= 0) {
        ::close(_fd_shm);
        shm_unlink(SHM_NAME.c_str());
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

    _fd_shm = shm_open(SHM_NAME.c_str(), O_CREAT | O_RDWR, 0666);
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

void SharedDictMaster::_initialize_metadata() {
    // This method initializes the metadata for the ring buffers and frames
    // such that clients can identify where to write frames and how to interpret the memory layout
    auto* layout = static_cast<Layout*>(_map);
    layout->num_buffers = _num_ringbuffers;

    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    char* current = static_cast<char*>(_map) + offsetof(Layout, buffers);
    for (const auto& config : _config.get_config().shared_memory) {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
        auto* buffer = reinterpret_cast<Buffer*>(current);

        new (&buffer->head) std::atomic<uint32_t>(0);
        new (&buffer->sequence) std::atomic<uint32_t>(0);
        std::strncpy(buffer->name, config.name.c_str(), sizeof(buffer->name) - 1);
        buffer->num_frames = config.num_frames;
        buffer->size_per_frame = config.size_per_frame;
        buffer->offset = static_cast<uint64_t>(current - static_cast<char*>(_map));

        const size_t frame_storage = offsetof(ImageFrame, data) + static_cast<size_t>(buffer->size_per_frame);
        const size_t buffer_storage = offsetof(Buffer, frames) + static_cast<size_t>(buffer->num_frames) * frame_storage;

        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
        current += buffer_storage;
        spdlog::info("Initialized buffer '{}', start {}, end {} bytes", config.name, buffer->offset, current - static_cast<char*>(_map));
    }
    _initialized.store(true);
}

} // namespace core
