#include "master.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <spdlog/spdlog.h>
#include <stdexcept>
#include <string>
#include <sys/mman.h>
#include <system_error>
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

SharedDictMaster::~SharedDictMaster() noexcept {
    if (_map != nullptr && _map != MAP_FAILED) {
        if (munmap(_map, _total_size) != 0) {
            spdlog::warn("munmap failed: {}", std::generic_category().message(errno));
        }
        _map = nullptr;
    }

    if (_fd_shm >= 0) {
        ::close(_fd_shm);
        _fd_shm = -1;
    }

    // Unlink the shared memory object to clean up the namespace
    if (shm_unlink(SHM_NAME.c_str()) != 0) {
        // If other processes still have it open, unlink might fail; log and continue
        spdlog::warn("shm_unlink('{}') failed: {}", SHM_NAME, std::generic_category().message(errno));
    }

    spdlog::info("SharedDictMaster resources cleaned up");
}

void SharedDictMaster::_initialize_shm() {
    const auto& rb_config = _config.get_config().shared_memory;
    _num_ringbuffers = rb_config.size();

    size_t total_size = offsetof(Layout, buffers);
    for (const auto& cfg : rb_config) {
        // Storage for one buffer header + N frames (each frame header + payload)
        const size_t frame_storage = offsetof(DataFrame, data) + static_cast<size_t>(cfg.size_per_frame);
        const size_t buffer_storage = offsetof(Buffer, frames) + static_cast<size_t>(cfg.num_frames) * frame_storage;

        // Track size_per_buffer for logging/diagnostics; last value kept
        _size_per_buffer = static_cast<uint32_t>(buffer_storage);
        total_size += buffer_storage;

        spdlog::info("size_per_buffer for '{}': {}", cfg.name, buffer_storage);
    }

    if (total_size > std::numeric_limits<uint32_t>::max()) {
        throw std::runtime_error("Shared memory size exceeds uint32_t capacity");
    }
    _total_size = static_cast<uint32_t>(total_size);

    // Create or open the shared memory object
    errno = 0;
    _fd_shm = shm_open(SHM_NAME.c_str(), O_CREAT | O_RDWR, 0666);
    if (_fd_shm < 0) {
        const auto msg = std::generic_category().message(errno);
        spdlog::error("Failed to create shared memory segment '{}': {}", SHM_NAME, msg);
        throw std::runtime_error("shm_open failed: " + msg);
    }

    // Resize to required total size
    if (ftruncate(_fd_shm, static_cast<off_t>(_total_size)) < 0) {
        const auto msg = std::generic_category().message(errno);
        spdlog::error("Failed to set size of shared memory segment '{}': {}", SHM_NAME, msg);
        ::close(_fd_shm);
        _fd_shm = -1;
        // Attempt unlink to avoid leaving a bad object
        shm_unlink(SHM_NAME.c_str());
        throw std::runtime_error("ftruncate failed: " + msg);
    }

    // Map the shared memory into our address space
    _map = mmap(nullptr, _total_size, PROT_READ | PROT_WRITE, MAP_SHARED, _fd_shm, 0);
    if (_map == MAP_FAILED) {
        const auto msg = std::generic_category().message(errno);
        spdlog::error("mmap failed for '{}': {}", SHM_NAME, msg);
        ::close(_fd_shm);
        _fd_shm = -1;
        shm_unlink(SHM_NAME.c_str());
        throw std::runtime_error("mmap failed: " + msg);
    }

    // Zero-initialize the entire region
    std::memset(_map, 0, _total_size);
    spdlog::info("Initialized shared memory '{}' with {} bytes", SHM_NAME, _total_size);
}

void SharedDictMaster::_initialize_metadata() {
    // Initialize metadata so clients can understand the layout
    auto* layout = static_cast<Layout*>(_map);
    layout->num_buffers = _num_ringbuffers;

    auto* base = static_cast<std::byte*>(_map);

    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    std::byte* current = base + offsetof(Layout, buffers);

    for (const auto& cfg : _config.get_config().shared_memory) {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
        auto* buffer = reinterpret_cast<Buffer*>(current);

        // Construct atomics in place
        new (&buffer->head) std::atomic<uint32_t>(0);
        new (&buffer->sequence) std::atomic<uint32_t>(0);

        // Safe copy of name with guaranteed null-termination
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-array-to-pointer-decay,hicpp-no-array-decay)
        std::memset(buffer->name, 0, sizeof(buffer->name));
        const auto copy_len = std::min(cfg.name.size(), sizeof(buffer->name) - 1);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-array-to-pointer-decay,hicpp-no-array-decay)
        std::memcpy(buffer->name, cfg.name.data(), copy_len);

        buffer->num_frames = cfg.num_frames;
        buffer->size_per_frame = cfg.size_per_frame;
        buffer->offset = static_cast<uint64_t>(current - base);

        const size_t frame_storage = offsetof(DataFrame, data) + static_cast<size_t>(buffer->size_per_frame);
        const size_t buffer_storage = offsetof(Buffer, frames) + static_cast<size_t>(buffer->num_frames) * frame_storage;

        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
        current += buffer_storage;

        spdlog::info("Initialized buffer '{}', start {} bytes, end {} bytes",
                     cfg.name, buffer->offset, static_cast<size_t>(current - base));
    }

    _initialized.store(true, std::memory_order_release);
}

} // namespace core
