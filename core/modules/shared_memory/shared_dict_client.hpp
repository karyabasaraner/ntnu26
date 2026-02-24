#ifndef WORKSPACES_CORE_CORE_MODULES_SHARED_MEMORY_SHARED_DICT_CLIENT_HPP
#define WORKSPACES_CORE_CORE_MODULES_SHARED_MEMORY_SHARED_DICT_CLIENT_HPP

#include "../utils/configs.hpp"
#include "ringbuffer.hpp"
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
#include <unordered_map>

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

    bool is_ready();
    void add(const std::string& key, const void* data, size_t length, uint64_t timestamp_ns);
    void initialize(CameraConfig config);

private:
    CameraConfig _config;
    int _fd_shm{-1};
    size_t _shm_total_size{0};
    std::atomic<bool> _stop{false};
    std::condition_variable _cv;
    std::mutex _queue_mutex;
    std::queue<DataEntry> _data_queue;
    std::thread _worker_thread;
    std::unique_ptr<core::Transform> _transform;
    void* _map{nullptr};
    std::unordered_map<std::string, Buffer*> _buffer_map;

    void _get_shm_structure();
    void _get_transforms_from_config();
    void _process_queue();
    void _start_processing();
    void _stop_processing();
    void _get_shm_map() ;
};

} // namespace core

#endif
