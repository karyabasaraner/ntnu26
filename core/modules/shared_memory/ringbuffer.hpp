#ifndef WORKSPACES_CORE_CORE_MODULES_SHARED_MEMORY_RINGBUFFER_HPP
#define WORKSPACES_CORE_CORE_MODULES_SHARED_MEMORY_RINGBUFFER_HPP


#include <atomic>
#include <cstdint>

namespace core {

struct ImageFrame {
    // One frame of image data, along with metadata
    uint64_t timestamp_ns{0};
    uint32_t checksum{0}; // simple checksum of the data for integrity checking
    uint32_t sequence{0}; // sequence number for this frame, incremented by writer

    //NOLINTNEXTLINE(hicpp-avoid-c-arrays,cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays)
    uint8_t data[];
};

struct Buffer {
    std::atomic<uint32_t> head{0};
    std::atomic<uint32_t> sequence{0};

    //NOLINTNEXTLINE(hicpp-avoid-c-arrays,cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays)
    char name[32]; // name of this buffer

    uint32_t num_frames; // number of frames in this buffer
    uint32_t size_per_frame; // size of frame in bytes
    uint64_t offset; // offset from shm base

    //NOLINTNEXTLINE(hicpp-avoid-c-arrays,cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays)
    ImageFrame frames[];
};

struct Layout {
    uint8_t num_buffers;

    //NOLINTNEXTLINE(hicpp-avoid-c-arrays,cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays)
    Buffer buffers[];
};

} // namespace core

#endif