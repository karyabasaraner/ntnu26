#include "client.hpp"

#include "configs.hpp"
#include "ringbuffer.hpp"
#include "utils.hpp"

#include <cstddef>
#include <cstdint>
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

// Overflow-safe helpers
// NOLINTNEXTLINE(readability-identifier-length)
inline bool mul_overflow(size_t a, size_t b, size_t& out) noexcept {
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_mul_overflow(a, b, &out);
#else
    if (b != 0 && a > std::numeric_limits<size_t>::max() / b) return true;
    out = a * b;
    return false;
#endif
}

// NOLINTNEXTLINE(readability-identifier-length)
inline bool add_overflow(size_t a, size_t b, size_t& out) noexcept {
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_add_overflow(a, b, &out);
#else
    if (a > std::numeric_limits<size_t>::max() - b) return true;
    out = a + b;
    return false;
#endif
}

SharedDictClient::SharedDictClient(std::string name) : _name(std::move(name)) {
    if (!_name.empty()) {
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
        return false;
    }

    if (_buffer == nullptr) {
        spdlog::warn("SharedDictClient is not ready: buffer '{}' not found in shared memory", _name);
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

uint32_t SharedDictClient::get_head(int32_t index_from_head) const {
    // index_from_head=0 means getting the current head, so empty
    // index_from_head=1 means getting the most recently completed frame, so one back from head
    if (_buffer == nullptr) {
        spdlog::error("{}: Cannot get requested head: buffer is null", _name);
        return 0;
    }

    const uint32_t num_frames = _buffer->num_frames;
    if (num_frames == 0) {
        spdlog::error("{}: num_frames is zero", _name);
        return 0;
    }

    const auto head = static_cast<int32_t>(_buffer->head.load()) - index_from_head;
    const auto requested = static_cast<uint32_t>((head % num_frames + num_frames) % num_frames);
    return requested;
}


DataFrame* SharedDictClient::get_frame_by_index(uint32_t head_index) {
    if (_buffer == nullptr) {
        spdlog::error("{}: Buffer is null", _name);
        return nullptr;
    }

    // Compute frame_stride = offsetof(DataFrame, data) + size_per_frame
    size_t frame_stride = 0;
    if (add_overflow(static_cast<size_t>(offsetof(DataFrame, data)),
                     static_cast<size_t>(_buffer->size_per_frame),
                     frame_stride)) {
        spdlog::error("{}: Frame stride overflows (header={} + payload={})", _name, offsetof(DataFrame, data), _buffer->size_per_frame);
        return nullptr;
    }

    // Compute frames_base = buffer->offset + offsetof(Buffer, frames)
    size_t frames_base = 0;
    if (add_overflow(static_cast<size_t>(_buffer->offset),
                     static_cast<size_t>(offsetof(Buffer, frames)),
                     frames_base)) {
        spdlog::error("{}: Frames base calculation overflow (offset={}, frames_off={})", _name, _buffer->offset, offsetof(Buffer, frames));
        return nullptr;
    }

    const size_t shm_size = get_shm_total_size();

    if (frames_base > shm_size) {
        spdlog::error("{}: Frames base offset {} is out of bounds for shared memory size {}", _name, frames_base, shm_size);
        return nullptr;
    }

    // offset_from_base = requested_head * frame_stride
    size_t offset_from_base = 0;
    if (mul_overflow(static_cast<size_t>(head_index), frame_stride, offset_from_base)) {
        spdlog::error("{}: Frame offset multiplication would overflow (head={}, stride={})", _name, head_index, frame_stride);
        return nullptr;
    }

    if (offset_from_base > shm_size - frames_base) {
        spdlog::error("{}: Computed frame offset exceeds shared memory (offset={}, base={}, shm={})", _name, offset_from_base, frames_base, shm_size);
        return nullptr;
    }

    size_t frame_offset = 0;
    if (add_overflow(frames_base, offset_from_base, frame_offset)) {
        spdlog::error("{}: Frame offset addition overflow (base={}, offset={})", _name, frames_base, offset_from_base);
        return nullptr;
    }

    if (frame_stride > shm_size - frame_offset) {
        spdlog::error("{}: Frame at offset {} (size {}) does not fully fit in shared memory of size {}", _name, frame_offset, frame_stride, shm_size);
        return nullptr;
    }

    auto* base = static_cast<std::byte*>(get_shm_map());
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic,cppcoreguidelines-pro-type-reinterpret-cast)
    auto* frame = reinterpret_cast<DataFrame*>(base + frame_offset);
    return frame;
}

void SharedDictClient::_get_shm_structure() {
    if (_map == nullptr) {
        spdlog::warn("Shared memory map is null, client will not be able to write to shared memory");
        return;
    }

    auto* layout = static_cast<Layout*>(_map);
    const size_t num_buffers = layout->num_buffers;

    auto cur_buffer_offset = static_cast<size_t>(offsetof(Layout, buffers));

    for (size_t i = 0; i < num_buffers; ++i) {
        if (cur_buffer_offset > _shm_total_size) {
            spdlog::error("Buffer offset {} is out of bounds for shared memory size {}", cur_buffer_offset, _shm_total_size);
            break;
        }

        // Before reading header fields, ensure header area fits
        size_t header_need = 0;
        if (add_overflow(cur_buffer_offset, static_cast<size_t>(offsetof(Buffer, frames)), header_need) ||
            header_need > _shm_total_size) {
            spdlog::error("Buffer header at offset {} does not fit in shared memory of size {}",
                          cur_buffer_offset, _shm_total_size);
            break;
        }

        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast,cppcoreguidelines-pro-bounds-pointer-arithmetic)
        auto* buffer = reinterpret_cast<Buffer*>(static_cast<std::byte*>(_map) + cur_buffer_offset);
        spdlog::info("{}: Found buffer with name {}", _name, buffer->name);

        /// NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-array-to-pointer-decay,hicpp-no-array-decay)
        if (buffer->name == _name) {
            _buffer = buffer;
            spdlog::info("{}: Matched buffer in shared memory with config name {}", _name, buffer->name);
            break;
        }

        // Compute size for the entire buffer block: offsetof(Buffer, frames) + num_frames * frame_stride
        size_t frame_stride = 0;
        if (add_overflow(static_cast<size_t>(offsetof(DataFrame, data)),
                         static_cast<size_t>(buffer->size_per_frame),
                         frame_stride)) {
            spdlog::error("Frame stride overflow while iterating buffers (header={} + payload={})", offsetof(DataFrame, data), buffer->size_per_frame);
            break;
        }

        size_t frames_bytes = 0;
        if (mul_overflow(static_cast<size_t>(buffer->num_frames), frame_stride, frames_bytes)) {
            spdlog::error("Frames bytes overflow: num_frames={} * stride={}", buffer->num_frames, frame_stride);
            break;
        }

        size_t buffer_content_size = 0;
        if (add_overflow(static_cast<size_t>(offsetof(Buffer, frames)), frames_bytes, buffer_content_size)) {
            spdlog::error("Buffer content size overflow while iterating buffers");
            break;
        }

        size_t next_offset = 0;
        if (add_overflow(cur_buffer_offset, buffer_content_size, next_offset)) {
            spdlog::error("Next buffer offset overflow: cur={} + size={}", cur_buffer_offset, buffer_content_size);
            break;
        }

        cur_buffer_offset = next_offset;
    }
}

void SharedDictClient::_get_shm_map() {
    _fd_shm = shm_open(SHM_NAME.c_str(), O_RDWR, 0666);
    if (_fd_shm < 0) {
        // This is not critical, just means we won't be able to write to shared memory.
        spdlog::warn("Failed to open shared memory segment in client");
        return;
    }

    struct stat shm_stats {};
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
