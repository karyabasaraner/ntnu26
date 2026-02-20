#ifndef CORE_MODULES_MEMORY_SHARED_DICT_HPP
#define CORE_MODULES_MEMORY_SHARED_DICT_HPP

#include "../utils/configs.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

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
    void set_config(CameraConfig config) { _config = std::move(config); }

private:
    struct DataEntry {
        std::string key;
        std::vector<uint8_t> data;
        uint64_t timestamp_ns;
    };

    std::queue<DataEntry> _data_queue;
    std::mutex _queue_mutex;
    std::thread _worker_thread;
    std::condition_variable _cv;
    std::atomic<bool> _stop{false};
    CameraConfig _config;

    void _process_queue();
    void _start_processing();
    void _stop_processing();

    void _tmp_cuda_convert(std::vector<uint8_t>& data);
};

} // namespace core

#endif
