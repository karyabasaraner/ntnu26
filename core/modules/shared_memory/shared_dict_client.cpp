#include "shared_dict_client.hpp"

#include <cstdint>
#include <mutex>
#include <spdlog/spdlog.h>
#include <string>

namespace core {

SharedDictClient::SharedDictClient() {
    _start_processing();
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
        std::lock_guard<std::mutex> lock(_queue_mutex);
        _stop = true;
    }
    _cv.notify_all();
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
            std::lock_guard<std::mutex> lock(_queue_mutex);
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

            // 4. Write to shared memory here (TODO)
        }

        // TODO(MJ): Implement actual shared memory writing here!
        spdlog::info("Processing entry with key: {}, data length: {}, timestamp: {}", entry.key, entry.data.size(), entry.timestamp_ns);
    }
}

} // namespace core
