#ifndef WORKSPACES_CORE_CORE_MODULES_CAMERA_BASE_CAMERA_HPP
#define WORKSPACES_CORE_CORE_MODULES_CAMERA_BASE_CAMERA_HPP

#include "../utils/configs.hpp"
#include "../shared_memory/client/writer.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <thread>

namespace core {

class Camera {
public:
    explicit Camera(CameraConfig config);

    // Delete copy and move
    Camera(const Camera&) = delete;
    Camera& operator=(const Camera&) = delete;
    Camera(Camera&&) = delete;
    Camera& operator=(Camera&&) = delete;

    // Destructor
    virtual ~Camera();

    bool is_running() const { return _running; }
    bool start();
    const CameraConfig& get_config() const { return _config; }
    virtual bool is_valid() const = 0;
    void process_frame(void* data, size_t length, uint32_t sequence, uint64_t timestamp_ns);
    void stop();

private:
    CameraConfig _config;
    SharedDictWriter _shdict_writer;
    std::atomic<bool> _running = false;
    std::thread _worker;

    virtual bool _start_acquisition();
    virtual bool _stop_acquisition();
    virtual void _capture_loop() = 0;
    virtual void _post_stop() {}  // Optional hook for derived classes to perform actions after stopping
    
};

} // namespace core

#endif