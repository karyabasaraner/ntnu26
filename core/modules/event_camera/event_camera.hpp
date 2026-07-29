#ifndef WORKSPACES_CORE_CORE_MODULES_EVENT_CAMERA_EVENT_CAMERA_HPP
#define WORKSPACES_CORE_CORE_MODULES_EVENT_CAMERA_EVENT_CAMERA_HPP

#include "../shared_memory/client/writer.hpp"
#include "../utils/configs.hpp"
#include "event_record.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <thread>

namespace core {

class EventCamera {
public:
    explicit EventCamera(EventCameraConfig config);

    // Delete copy and move
    EventCamera(const EventCamera&) = delete;
    EventCamera& operator=(const EventCamera&) = delete;
    EventCamera(EventCamera&&) = delete;
    EventCamera& operator=(EventCamera&&) = delete;

    virtual ~EventCamera() = default;

    bool is_running() const { return _running; }
    bool start();
    virtual bool is_valid() const = 0;
    void stop() noexcept;

protected:
    const EventCameraConfig& get_config() const { return _config; }

    // Publishes a batch of raw events as one or more shared-memory frames. Batches
    // larger than the configured max_events_per_frame capacity are split across
    // multiple frames (each a verbatim slice of the input); no events are dropped
    // or accumulated/merged. Each frame's own timestamp is the first event's
    // timestamp in that frame's slice.
    void process_events(const EventRecord* events, size_t count);

private:
    enum class State : uint8_t {
        initialized,
        running,
        stopped,
    };

    EventCameraConfig _config;
    SharedDictWriter _shdict_writer;
    std::atomic<bool> _running = false;
    uint32_t _sequence{0};
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
