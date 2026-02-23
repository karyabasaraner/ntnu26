#include "shared_dict_client.hpp"

#include "configs.hpp"
#include "transforms.hpp"
#include "utils.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <spdlog/spdlog.h>
#include <string>
#include <utility>
#include <vector>


namespace core {

SharedDictClient::SharedDictClient() {
    // NOTE: We don't have the config at construction, so we can't start processing yet
    spdlog::info("SharedDictClient constructed, waiting for initialization");
}

SharedDictClient::~SharedDictClient() {
    _stop_processing();
    _cv.notify_all();
    if (_worker_thread.joinable()) {
        _worker_thread.join();
    }
}

void SharedDictClient::_start_processing() {
    spdlog::info("Starting SharedDictClient worker thread");
    _worker_thread = std::thread(&SharedDictClient::_process_queue, this);
}

void SharedDictClient::_stop_processing() {
    spdlog::info("Stopping SharedDictClient worker thread");
    {
        std::lock_guard<std::mutex> const lock(_queue_mutex);
        _stop = true;
    }
    _cv.notify_all();
}

void SharedDictClient::initialize(CameraConfig config) {
    _config = std::move(config);
    _get_transforms_from_config();

    _start_processing();
}

void SharedDictClient::add(const std::string& key, const void* data, size_t length, uint64_t timestamp_ns) {
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

void SharedDictClient::_process_queue() {
    // NOTE: (Post-) processing can be done here
    while(true) {
        DataEntry entry;
        {
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

        }

        // TODO(MJ): Implement actual shared memory writing here!
        spdlog::info("Processing entry with key: {}, data length: {}, timestamp: {}", entry.key, entry.data.size(), entry.timestamp_ns);
    }
}

void SharedDictClient::_get_transforms_from_config() {
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

} // namespace core
