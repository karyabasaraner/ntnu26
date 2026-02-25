#include "shared_dict_client.hpp"

#include "transforms.hpp"
#include "ringbuffer.hpp"

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>


namespace core {

class SharedDictClientWriter : SharedDictClient {
public:
    explicit SharedDictClientWriter(CameraConfig config={});
    ~SharedDictClientWriter();

    SharedDictClientWriter(const SharedDictClientWriter&) = delete;
    SharedDictClientWriter(SharedDictClientWriter&&) = delete;
    SharedDictClientWriter& operator=(const SharedDictClientWriter&) = delete;
    SharedDictClientWriter& operator=(SharedDictClientWriter&&) = delete;

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

    ImageFrame* _get_next_image_frame();
    void _get_transforms_from_config();
    void _process_queue();
    void _start_processing();
    void _stop_processing();
    uint32_t _get_current_head() const;
};

} // namespace core
