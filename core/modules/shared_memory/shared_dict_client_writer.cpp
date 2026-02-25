#include "shared_dict_client_writer.hpp"

#include "configs.hpp"
#include "ringbuffer.hpp"
#include "shared_dict_client.hpp"
#include "transforms.hpp"
#include "utils.hpp"
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <spdlog/spdlog.h>
#include <utility>
#include <vector>
#include <zconf.h>
#include <zlib.h>

namespace core {

SharedDictClientWriter::SharedDictClientWriter(CameraConfig config) : SharedDictClient(std::move(config)) {
    _config = get_config();
    if (!_config.name.empty()) {
        _get_transforms_from_config();
        _start_processing();

        _buffer = get_buffer();
    }
}

SharedDictClientWriter::~SharedDictClientWriter() {
    _stop_processing();
    _cv.notify_all();
    if (_worker_thread.joinable()) {
        _worker_thread.join();
    }
}

void SharedDictClientWriter::add(const std::string& key, const void* data, size_t length, uint64_t timestamp_ns) {
    // NOTE: This blocks the caller, so no heavy processing here!
    if (length > 0) {
        if (data == nullptr) {
            spdlog::warn("Attempted to add data with non-zero length but null pointer for key: {}", key);
            return;
        }

        std::vector<uint8_t> buf(length);
        std::memcpy(buf.data(), data, length);

        // Enqueue the data for processing
        DataEntry entry{key, std::move(buf), timestamp_ns};
        {
            std::lock_guard<std::mutex> const lock(_queue_mutex);
            if (_stop) {
                spdlog::warn("Stopped, not adding data: {}", key);
                return;
            }

            _data_queue.push(std::move(entry));
        }

        // Notify the worker thread that we have new data
        _cv.notify_one();

    } else {
        spdlog::warn("Attempted to add data with zero length for key: {}", key);
    }
}

void SharedDictClientWriter::_start_processing() {
    spdlog::info("Starting SharedDictClient worker thread");
    _worker_thread = std::thread(&SharedDictClientWriter::_process_queue, this);
}

void SharedDictClientWriter::_stop_processing() {
    spdlog::info("Stopping SharedDictClient worker thread");
    {
        std::lock_guard<std::mutex> const lock(_queue_mutex);
        _stop = true;
    }
    _cv.notify_all();
}

void SharedDictClientWriter::_process_queue() {
    // NOTE: (Post-) processing can be done here
    while(true) {
        DataEntry entry;
        {
            // 0. Check if buffer is ready
            if (!is_ready()) {
                spdlog::warn("SharedDictClientWriter is not ready, waiting before processing queue");
                continue;
            }

            // 1. Acquire lock and wait for data (or stop)
            std::unique_lock<std::mutex> lock(_queue_mutex);
            _cv.wait(lock, [this] { return _stop || !_data_queue.empty(); });

            // 2. Check if we should stop
            if (_stop && _data_queue.empty()) {
                spdlog::info("Stopping SharedDictClient worker thread");
                return;
            }

            // 3. We have data to process, pop it from the queue
            entry = std::move(_data_queue.front());
            _data_queue.pop();

            // 4. Apply all transforms
            if (_transform != nullptr) {
                spdlog::info("Applying transforms to entry with key: {}", entry.key);
                _transform->apply(entry);
            }

            // 5. Write the processed data to shared memory
            ImageFrame* frame = _get_next_image_frame();
            if (frame == nullptr) {
                spdlog::error("Failed to get next image frame for writing, skipping entry with key: {}", entry.key);
                continue;
            }
            frame->timestamp_ns = entry.timestamp_ns;
            if (entry.data.size() > _buffer->size_per_frame) {
                spdlog::error("Data size {} exceeds buffer size per frame {}, skipping entry with key: {}", entry.data.size(), _buffer->size_per_frame, entry.key);
                continue;
            }
            // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-array-to-pointer-decay,hicpp-no-array-decay)
            std::memcpy(frame->data, entry.data.data(), entry.data.size());
            frame->checksum = crc32(0, entry.data.data(), static_cast<uInt>(entry.data.size()));

            // Update the head and sequence atomics to indicate a new frame is available
            // since this atomic, and this is the only writer, it should be safe
            std::atomic_thread_fence(std::memory_order_release);
            _buffer->sequence.fetch_add(1, std::memory_order_release);
            _buffer->head.fetch_add(1, std::memory_order_release);
        }

        spdlog::info("Wrote entry: {}, seq: {}, head: {}, timestamp: {}", entry.key, _buffer->sequence.load()-1, _buffer->head.load(), entry.timestamp_ns);
    }
}

void SharedDictClientWriter::_get_transforms_from_config() {
    std::unique_ptr<Transform> chain = nullptr;
    for (const auto& config : _config.transforms) {
        if (config.name == "UYVY2RGB") {
            chain = std::make_unique<UYVY2RGB>(_config.width, _config.height, config, std::move(chain));
            spdlog::info("Added UYVY2RGB transform to transform chain");
        } else {
            spdlog::warn("Unknown transform in config: {}", config.name);
        }
    }
    _transform = std::move(chain);
}

ImageFrame* SharedDictClientWriter::_get_next_image_frame() {
    if (_buffer == nullptr) {
        spdlog::error("{}: Buffer is null", _config.name);
        return nullptr;
    }

    // Current head: confirmed written frames, we want to write next
    const uint32_t current_head = _get_current_head();

    const uint64_t next_frame_offset = _buffer->offset + offsetof(Buffer, frames) + current_head * (offsetof(ImageFrame, data) + _buffer->size_per_frame);
    if (next_frame_offset + offsetof(ImageFrame, data) + _buffer->size_per_frame > get_shm_total_size()) {
        spdlog::error("Next frame offset {} is out of bounds for shared memory size {}", next_frame_offset, get_shm_total_size());
        return nullptr;
    };

    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic,cppcoreguidelines-pro-type-reinterpret-cast)
    auto* frame = reinterpret_cast<ImageFrame*>(static_cast<char*>(get_shm_map()) + next_frame_offset);
    return frame;
}

uint32_t SharedDictClientWriter::_get_current_head() const {
    if (_buffer == nullptr) {
        spdlog::error("Cannot get current head: buffer is null");
        return 0;
    }
    uint32_t current_head = _buffer->head.load();
    if (current_head >= _buffer->num_frames) {
        current_head = current_head % _buffer->num_frames;
    }
    return current_head;
}

} // namespace core