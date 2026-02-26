#include "shared_dict_writer.hpp"

#include "configs.hpp"
#include "ringbuffer.hpp"
#include "shared_dict_client.hpp"
#include "transforms.hpp"
#include "utils.hpp"
#include <atomic>
#include <chrono>
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

SharedDictWriter::SharedDictWriter(CameraConfig config) : SharedDictClient(std::move(config)) {
    _config = get_config();

    if (_config.name.empty()) {
        spdlog::warn("SharedDictWriter constructed with empty config name; writer not started");
        return;
    }

    // Build transform chain first
    _get_transforms_from_config();

    // Obtain buffer before starting the worker to avoid races on readiness
    _buffer = get_buffer();
    if (_buffer == nullptr) {
        spdlog::error("Failed to acquire shared buffer for '{}'; writer not started", _config.name);
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

void SharedDictWriter::add(const std::string& key, const void* data, size_t length, uint64_t timestamp_ns) {
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
        _data_queue.push(DataEntry{key, std::move(buf), 0, timestamp_ns});
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

        // 2) Apply transforms outside the queue lock
        if (_transform) {
            _transform->apply(entry);
        }

        // 3) Write to shared memory
        ImageFrame* frame = get_requested_frame();
        if (frame == nullptr) {
            spdlog::error("Failed to get next image frame for writing; skipping key: {}", entry.key);
            continue;
        }

        if (entry.data.size() > _buffer->size_per_frame) {
            spdlog::error(
                "Data size {} exceeds buffer size per frame {}; skipping key: {}",
                entry.data.size(), _buffer->size_per_frame, entry.key
            );
            continue;
        }

        frame->timestamp_ns = entry.timestamp_ns;

        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-array-to-pointer-decay,hicpp-no-array-decay)
        std::memcpy(frame->data, entry.data.data(), entry.data.size());
        frame->checksum = crc32(0, entry.data.data(), static_cast<uInt>(entry.data.size()));

        // Publish: get current sequence (pre-increment), write it to the frame, then advance sequence and head.
        // Use release ordering to make frame contents visible to readers that use acquire.
        const auto seq = _buffer->sequence.fetch_add(1, std::memory_order_release);
        frame->sequence = seq;
        _buffer->head.fetch_add(1, std::memory_order_release);

        const auto now_ns = std::chrono::steady_clock::now().time_since_epoch().count();
        const auto dt_frame_now_ms = static_cast<double>(now_ns - entry.timestamp_ns) / 1e6;
        spdlog::debug(
            "Wrote entry: {}, seq: {}, head: {}, timestamp: {}, dt: {} ms",
            entry.key,
            seq,
            _buffer->head.load(std::memory_order_relaxed),
            entry.timestamp_ns,
            dt_frame_now_ms
        );
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