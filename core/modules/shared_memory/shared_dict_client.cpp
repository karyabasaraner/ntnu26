#include "shared_dict_client.hpp"

#include "configs.hpp"
#include "ringbuffer.hpp"
#include "utils.hpp"

#include <cstddef>
#include <cstring>
#include <fcntl.h>
#include <spdlog/spdlog.h>
#include <stdexcept>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>


namespace core {

SharedDictClient::SharedDictClient(CameraConfig config) {
    if (!config.name.empty()) {
        _config = std::move(config);

        _get_shm_map();
        _get_shm_structure();
    }
}


SharedDictClient::~SharedDictClient() {
    if (_map != nullptr) {
        munmap(_map, _shm_total_size);
    }

    if (_fd_shm >= 0) {
        ::close(_fd_shm);
    }
}

bool SharedDictClient::is_ready() const {
    if (_map == nullptr) {
        spdlog::warn("SharedDictClient is not ready: shared memory map is null");
        return false;
    }

    if (_buffer == nullptr) {
        spdlog::warn("SharedDictClient is not ready: buffer '{}' not found in shared memory", _config.name);
        return false;
    }

    return true;
}

Buffer* SharedDictClient::get_buffer() const {
    if (!is_ready()) {
        spdlog::warn("SharedDictClient is not ready, cannot get buffer");
        return nullptr;
    }

    return _buffer;
}

void SharedDictClient::_get_shm_structure() {
    if (_map == nullptr) {
        spdlog::warn("Shared memory map is null, client will not be able to write to shared memory");
        return;
    }

    auto* layout = static_cast<Layout*>(_map);
    const auto num_buffers = layout->num_buffers;
    auto cur_buffer_offset = offsetof(Layout, buffers);
    for (size_t i = 0; i < num_buffers; ++i) {
        if (cur_buffer_offset > _shm_total_size) {
            spdlog::error("Buffer offset {} is out of bounds for shared memory size {}", cur_buffer_offset, _shm_total_size);
            break;
        }

        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast,cppcoreguidelines-pro-bounds-pointer-arithmetic)
        auto* buffer = reinterpret_cast<Buffer*>(static_cast<char*>(_map) + cur_buffer_offset);
        spdlog::info("{}: Found buffer with name {}", _config.name, buffer->name);

        /// NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-array-to-pointer-decay,hicpp-no-array-decay)
        if (buffer->name == _config.name) {
            _buffer = buffer;
            spdlog::info("{}: Matched buffer in shared memory with config name {}", _config.name, buffer->name);
            break;
        }

        // Move to the next buffer based on the size of the current buffer
        const auto buffer_content_size = offsetof(Buffer, frames) + buffer->num_frames * (offsetof(ImageFrame, data) + buffer->size_per_frame);
        cur_buffer_offset += buffer_content_size;
    }
}

void SharedDictClient::_get_shm_map() {
    _fd_shm = shm_open(SHM_NAME.c_str(), O_RDWR, 0666);
    if (_fd_shm < 0) {
        // This is not critical, just means we won't be able to write to shared memory.
        spdlog::warn("Failed to open shared memory segment in client");
        return;
    }

    struct stat shm_stats{};
    if (fstat(_fd_shm, &shm_stats) < 0) {
        ::close(_fd_shm);
        spdlog::error("fstat failed on shm fd");
        // This is critical, as shm exists, but we can't get its size.
        throw std::runtime_error("fstat failed on shm fd");
    }
    _shm_total_size = static_cast<size_t>(shm_stats.st_size);

    _map = mmap(nullptr, _shm_total_size, PROT_READ | PROT_WRITE, MAP_SHARED, _fd_shm, 0);
    if (_map == MAP_FAILED) {
        ::close(_fd_shm);
        spdlog::error("mmap failed in client");
        throw std::runtime_error("mmap failed in client");
    }

    spdlog::info("Client mapped shared memory: {} bytes", _shm_total_size);
}

} // namespace core
