#ifndef WORKSPACES_CORE_CORE_MODULES_CAMERA_BASE_CAMERA_HPP
#define WORKSPACES_CORE_CORE_MODULES_CAMERA_BASE_CAMERA_HPP

#include "../utils/configs.hpp"
#include "../shared_memory/client/writer.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
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

    virtual ~Camera() = default;

    bool is_running() const { return _running; }
    bool start();
    virtual bool is_valid() const = 0;
    void stop() noexcept;

protected:
    const CameraConfig& get_config() const { return _config; }
    void process_frame(void* data, size_t length, uint32_t sequence, uint64_t timestamp_ns);

private:
    enum class State : uint8_t {
        initialized,
        running,
        stopped,
    };

    CameraConfig _config;
    SharedDictWriter _shdict_writer;
    std::atomic<bool> _running = false;
    State _state = State::initialized;
    std::mutex _state_mutex;
    std::thread _worker;

    virtual bool _start_acquisition() = 0;
    virtual bool _stop_acquisition() noexcept = 0;
    virtual void _capture_loop() = 0;
    virtual void _post_stop() noexcept = 0;
};

} // namespace core

#endif
