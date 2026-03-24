#include "client.hpp"
#include "configs.hpp"
#include "ringbuffer.hpp"
#include "transforms.hpp"
#include "utils.hpp"
#include "writer.hpp"

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

SharedDictWriter::SharedDictWriter(std::string name, WriterConfig config) : SharedDictClient(name), _name(std::move(name)), _config(std::move(config)) {
    if (_name.empty()) {
        spdlog::warn("SharedDictWriter constructed with empty config name; writer not started");
        return;
    }

    // Build transform chain first
    _get_transforms_from_config();

    // Obtain buffer before starting the worker to avoid races on readiness
    _buffer = get_buffer();
    if (_buffer == nullptr) {
        spdlog::error("Failed to acquire shared buffer for '{}'; writer not started", _name);
        return;
    }

    _start_processing();
}

SharedDictWriter::~SharedDictWriter() {
    _stop_processing();
    if (_worker_thread.joinable()) {
        _worker_thread.join();
    }
}

void SharedDictWriter::add(const std::string& key, const void* data, size_t length, uint32_t sequence, uint64_t timestamp_ns) {
    // Keep this fast: copy and enqueue only.
    if (length == 0) {
        spdlog::warn("Attempted to add data with zero length for key: {}", key);
        return;
    }
    if (data == nullptr) {
        spdlog::warn("Attempted to add data with non-zero length but null pointer for key: {}", key);
        return;
    }

    std::vector<std::uint8_t> buf(length);
    std::memcpy(buf.data(), data, length);
    {
        std::lock_guard<std::mutex> const lock(_queue_mutex);
        if (_stop) {
            spdlog::warn("Writer stopped; not adding data: {}", key);
            return;
        }
        _data_queue.push(DataEntry{key, std::move(buf), 0, sequence, timestamp_ns});
    }
    _cv.notify_one();
}

void SharedDictWriter::_start_processing() {
    if (_worker_thread.joinable()) {
        spdlog::warn("SharedDictWriter worker thread already running");
        return;
    }
    spdlog::info("Starting SharedDictWriter worker thread");
    _stop = false;
    _worker_thread = std::thread(&SharedDictWriter::_process_queue, this);
}

void SharedDictWriter::_stop_processing() {
    {
        std::lock_guard<std::mutex> const lock(_queue_mutex);
        if (_stop) {
            return;
        }
        _stop = true;
    }
    _cv.notify_all();
}

void SharedDictWriter::_process_queue() {
    for (;;) {
        DataEntry entry;

        // 1) Wait for work (or stop). Keep lock held only around queue ops.
        {
            std::unique_lock<std::mutex> lock(_queue_mutex);
            _cv.wait(lock, [this] {
                return _stop || (! _data_queue.empty() && is_ready());
            });

            if (_stop && _data_queue.empty()) {
                spdlog::info("Stopping SharedDictWriter worker thread");
                return;
            }

            entry = std::move(_data_queue.front());
            _data_queue.pop();
        }

        if (entry.data.size() > _buffer->size_per_frame) {
            spdlog::error(
                "Data size {} exceeds buffer size per frame {}; skipping key: {}",
                entry.data.size(), _buffer->size_per_frame, entry.key
            );
            continue;
        }

        // 2) Apply transforms outside the queue lock
        if (_transform) {
            _transform->apply(entry);
        }

        // 3) Write to shared memory, index_from_head = 0 -> next empty frame
        const uint32_t next_head = get_head(0);
        DataFrame* frame = get_frame_by_index(next_head);
        if (frame == nullptr) {
            spdlog::error("Failed to get next image frame for writing; skipping key: {}", entry.key);
            continue;
        }
        entry.head = next_head;

        frame->timestamp_ns = entry.timestamp_ns;

        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-array-to-pointer-decay,hicpp-no-array-decay)
        std::memcpy(frame->data, entry.data.data(), entry.data.size());
        frame->checksum = crc32(0, entry.data.data(), static_cast<uInt>(entry.data.size()));
        frame->sequence = entry.sequence;

        // Advance head
        const auto new_head = get_head(-1);
        _buffer->head.store(new_head, std::memory_order_release);
        _buffer->sequence.store(entry.sequence, std::memory_order_release);

        // Publish: get current sequence (pre-increment), write it to the frame, then advance sequence and head.
        // Use release ordering to make frame contents visible to readers that use acquire.
        // spdlog::debug(
        //     "Wrote {}, seq {}, head {}",
        //     entry.key,
        //     entry.sequence,
        //     next_head
        // );
    }
}

void SharedDictWriter::_get_transforms_from_config() {
    std::unique_ptr<Transform> chain = nullptr;
    for (const auto& cfg : _config.transforms) {
        if (cfg.name == "UYVY2RGB") {
            chain = std::make_unique<UYVY2RGB>(_config.width, _config.height, cfg, std::move(chain));
            spdlog::info("Added UYVY2RGB transform to transform chain");
        } else {
            spdlog::warn("Unknown transform in config: {}", cfg.name);
        }
    }
    _transform = std::move(chain);
}

} // namespace core
