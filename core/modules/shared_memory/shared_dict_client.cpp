#include "shared_dict_client.hpp"

#include "configs.hpp"
#include "ringbuffer.hpp"
#include "transforms.hpp"
#include "utils.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <mutex>
#include <spdlog/spdlog.h>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
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

    if (_map != nullptr) {
        munmap(_map, _shm_total_size);
    }

    if (_fd_shm >= 0) {
        ::close(_fd_shm);
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
    _get_shm_structure();
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

bool SharedDictClient::is_ready() {
    if (_map == nullptr) {
        spdlog::warn("SharedDictClient is not ready: shared memory map is null");
        return false;
    }

    if (_buffer_map.empty()) {
        spdlog::warn("SharedDictClient is not ready: no buffers found in shared memory");
        return false;
    }

    if (_buffer_map.find(_config.name) == _buffer_map.end()) {
        spdlog::warn("SharedDictClient is not ready: buffer '{}' not found in shared memory", _config.name);
        return false;
    }

    return true;
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

void SharedDictClient::_get_shm_structure() {
    // Sets map, fd and total size
    _get_shm_map();

    if (_map == nullptr) {
        spdlog::warn("Shared memory map is null, client will not be able to write to shared memory");
        return;
    }

    auto* layout = static_cast<Layout*>(_map);
    const auto num_buffers = layout->num_buffers;
    for (size_t i = 0; i < num_buffers; ++i) {
        const auto name = std::string(layout->buffers[i].name);
        _buffer_map[name] = &layout->buffers[i];
        spdlog::info("Got buffer: {}", name);
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
