#include "shared_dict_client.hpp"

#include "ringbuffer.hpp"
#include "transforms.hpp"

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <queue>
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

    void add(const std::string& key, const void* data, size_t length, uint64_t timestamp_ns);

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
