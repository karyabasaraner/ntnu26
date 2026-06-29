#ifndef WORKSPACES_CORE_CORE_MODULES_CAMERA_V4L2CAMERA_HPP
#define WORKSPACES_CORE_CORE_MODULES_CAMERA_V4L2CAMERA_HPP

#include "../utils/configs.hpp"
#include "../shared_memory/client/writer.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace core {

class V4L2Camera {
public:
    explicit V4L2Camera(CameraConfig config);

    // Delete copy and move
    V4L2Camera(const V4L2Camera&) = delete;
    V4L2Camera& operator=(const V4L2Camera&) = delete;
    V4L2Camera(V4L2Camera&&) = delete;
    V4L2Camera& operator=(V4L2Camera&&) = delete;

    // Destructor
    ~V4L2Camera();

    bool is_running() const { return _running; }
    bool is_valid() const { return _file_desc >= 0; };
    bool start();
    void stop();

private:
    CameraConfig _config;
    int _file_desc = -1;
    static const std::unordered_map<std::string, uint32_t> FOURCC_FORMATS;
    std::atomic<bool> _running = false;
    std::thread _worker;

    // Buffer struct for memory mapping
    struct Buffer {
        void* start;
        size_t length;
    };
    std::vector<Buffer> _buffers;

    SharedDictWriter _shdict_writer;

    bool _close_device();
    bool _configure() const;
    bool _init_mmap();
    bool _open_device();
    void _capture_loop();
    void _process_frame(void* data, size_t length, uint32_t sequence, uint64_t timestamp_ns);
    std::string _get_ctrl_name(uint32_t ctrl_id) const;
};

} // namespace core

#endif
