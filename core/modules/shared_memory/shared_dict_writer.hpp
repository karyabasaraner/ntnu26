#ifndef WORKSPACES_CORE_CORE_MODULES_SHARED_MEMORY_SHARED_DICT_WRITER_HPP
#define WORKSPACES_CORE_CORE_MODULES_SHARED_MEMORY_SHARED_DICT_WRITER_HPP

#include "configs.hpp"
#include "shared_dict_client.hpp"

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


namespace core {

class SharedDictWriter : public SharedDictClient {
public:
    explicit SharedDictWriter(CameraConfig config={});
    ~SharedDictWriter();

    SharedDictWriter(const SharedDictWriter&) = delete;
    SharedDictWriter(SharedDictWriter&&) = delete;
    SharedDictWriter& operator=(const SharedDictWriter&) = delete;
    SharedDictWriter& operator=(SharedDictWriter&&) = delete;

    void add(const std::string& key, const void* data, size_t length, uint32_t sequence, uint64_t timestamp_ns);

private:
    Buffer* _buffer{nullptr};
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
