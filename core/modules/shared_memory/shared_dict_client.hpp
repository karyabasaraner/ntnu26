#ifndef WORKSPACES_CORE_CORE_MODULES_SHARED_MEMORY_SHARED_DICT_CLIENT_HPP
#define WORKSPACES_CORE_CORE_MODULES_SHARED_MEMORY_SHARED_DICT_CLIENT_HPP

#include "../utils/configs.hpp"
#include "transforms.hpp"
#include "utils.hpp"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>

namespace core {

class SharedDictClient {
public:
    SharedDictClient();

    // TODO(MJ): Complete rule of five
    SharedDictClient(const SharedDictClient&) = delete;
    SharedDictClient& operator=(const SharedDictClient&) = delete;
    SharedDictClient(SharedDictClient&&) = delete;
    SharedDictClient& operator=(SharedDictClient&&) = delete;
    ~SharedDictClient();

    void add(const std::string& key, const void* data, size_t length, uint64_t timestamp_ns);
    void initialize(CameraConfig config);

private:
    CameraConfig _config;
    std::atomic<bool> _stop{false};
    std::condition_variable _cv;
    std::mutex _queue_mutex;
    std::queue<DataEntry> _data_queue;
    std::thread _worker_thread;
    std::unique_ptr<core::Transform> _transform;

    void _get_transforms_from_config();
    void _process_queue();
    void _start_processing();
    void _stop_processing();
};

} // namespace core

#endif

    // _layout = static_cast<Layout*>(map);
    // _layout->num_buffers = _num_ringbuffers;
    // _layout->size_per_buffer = _size_per_buffer;

    // // Initialize every buffer
    // auto* current_buffer = static_cast<char*>(map) + offsetof(Layout, buffers);
    // for (const auto& config : rb_config) {
    //     auto* buffer = reinterpret_cast<Buffer*>(current_buffer);

    //     // Initialize buffer metadata
    //     new (&buffer->head) std::atomic<uint32_t>(0);
    //     new (&buffer->sequence) std::atomic<uint32_t>(0);
    //     buffer->num_frames = config.num_frames;
    //     buffer->size_per_frame = config.size_per_frame;

    //     const auto offset = static_cast<uint64_t>(current_buffer - static_cast<char*>(map));
    //     if (offset < 0) {
    //         spdlog::error("Calculated negative offset for buffer '{}'", config.name);
    //         throw std::runtime_error("Calculated negative offset for buffer");
    //     }
    //     buffer->offset = offset;
    //     spdlog::info("Initialized buffer '{}' at start {} and end {} bytes", config.name, buffer->offset, buffer->offset + _size_per_buffer);
    //     current_buffer += offsetof(Buffer, frames);

    //     for (uint32_t i = 0; i < buffer->num_frames; ++i) {
    //         auto* frame = reinterpret_cast<ImageFrame*>(current_buffer);
    //         frame->timestamp_ns = 0;

    //         const auto offset = static_cast<uint64_t>(current_buffer - static_cast<char*>(map));
    //         if (offset < 0) {
    //             spdlog::error("Calculated negative offset for frame {} in buffer '{}'", i, config.name);
    //             throw std::runtime_error("Calculated negative offset for frame");
    //         }
    //         frame->offset = offset;
    //         current_buffer += sizeof(ImageFrame) + buffer->size_per_frame;
    //     }
    // }
