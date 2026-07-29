#ifndef WORKSPACES_CORE_CORE_MODULES_EVENT_CAMERA_PROPHESEE_EVENT_CAMERA_HPP
#define WORKSPACES_CORE_CORE_MODULES_EVENT_CAMERA_PROPHESEE_EVENT_CAMERA_HPP

#include "configs.hpp"
#include "event_camera.hpp"
#include "event_record.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

namespace core {

// Prophesee event camera backend on top of the Metavision SDK's C++ driver API
// (Metavision::Camera). Delivers raw CD (change-detection) events with no
// accumulation or loss: every Metavision::EventCD the SDK hands us is converted to
// an EventRecord and published via EventCamera::process_events().
class PropheseeEventCamera final : public EventCamera {
public:
    explicit PropheseeEventCamera(EventCameraConfig config);
    ~PropheseeEventCamera() override;

    PropheseeEventCamera(const PropheseeEventCamera&) = delete;
    PropheseeEventCamera& operator=(const PropheseeEventCamera&) = delete;
    PropheseeEventCamera(PropheseeEventCamera&&) = delete;
    PropheseeEventCamera& operator=(PropheseeEventCamera&&) = delete;

    bool is_valid() const override;

private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
    // Anchors the Metavision device clock (microseconds since stream start) onto the
    // same steady_clock domain the rest of core (Pylon fallback, IMU) publishes in.
    // Set on the first CD event received; see _on_cd_events() for the conversion.
    std::atomic<bool> _clock_anchored{false};
    int64_t _device_start_us{0};
    uint64_t _host_start_ns{0};
    std::vector<EventRecord> _conversion_buffer;

    static std::unique_ptr<Impl> _create_impl();
    bool _start_acquisition() override;
    bool _stop_acquisition() noexcept override;
    void _capture_loop() override;
    void _post_stop() noexcept override;
    void _on_cd_events(const void* begin, const void* end);
};

} // namespace core

#endif
