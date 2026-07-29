#ifdef CORE_ENABLE_METAVISION

#include "prophesee_event_camera.hpp"

#include "configs.hpp"
#include "event_camera.hpp"
#include "event_record.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

// NOTE(verify-on-device): method names below (from_first_available/from_serial,
// cd().add_callback, biases().set_from_file) match the Metavision SDK 5.x C++
// driver API as documented, but this file has never been compiled against the
// real SDK (not installed in the dev sandbox this was written in). Check these
// against the headers actually installed on the Jetson before trusting this file.
#include <metavision/sdk/base/events/event_cd.h>
#include <metavision/sdk/driver/camera.h>
#include <spdlog/spdlog.h>

namespace core {
namespace {

uint64_t steady_time_ns() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()
    ).count());
}

} // namespace

struct PropheseeEventCamera::Impl {
    std::unique_ptr<Metavision::Camera> camera;
};

std::unique_ptr<PropheseeEventCamera::Impl> PropheseeEventCamera::_create_impl() {
    return std::make_unique<Impl>();
}

PropheseeEventCamera::PropheseeEventCamera(EventCameraConfig config) :
    EventCamera(std::move(config)), _impl(_create_impl()) {
    const PropheseeCameraConfig& prophesee_config = get_config().prophesee;

    try {
        _impl->camera = std::make_unique<Metavision::Camera>(
            prophesee_config.serial_number.empty()
                ? Metavision::Camera::from_first_available()
                : Metavision::Camera::from_serial(prophesee_config.serial_number)
        );

        if (!prophesee_config.bias_file.empty()) {
            _impl->camera->biases().set_from_file(prophesee_config.bias_file);
        }

        _impl->camera->cd().add_callback([this](const Metavision::EventCD* begin, const Metavision::EventCD* end) {
            _on_cd_events(static_cast<const void*>(begin), static_cast<const void*>(end));
        });

        spdlog::info(
            "Opened Prophesee event camera '{}'{}",
            get_config().name,
            prophesee_config.serial_number.empty() ? "" : " (serial " + prophesee_config.serial_number + ")"
        );
    } catch (const std::exception& error) {
        spdlog::error("Failed to open Prophesee event camera '{}': {}", get_config().name, error.what());
        throw;
    }
}

PropheseeEventCamera::~PropheseeEventCamera() {
    stop();
}

bool PropheseeEventCamera::is_valid() const {
    return _impl && _impl->camera != nullptr;
}

bool PropheseeEventCamera::_start_acquisition() {
    try {
        _impl->camera->start();
        return true;
    } catch (const std::exception& error) {
        spdlog::error("Failed to start Prophesee event camera '{}': {}", get_config().name, error.what());
        return false;
    }
}

bool PropheseeEventCamera::_stop_acquisition() noexcept {
    try {
        if (_impl && _impl->camera) {
            _impl->camera->stop();
        }
        return true;
    } catch (const std::exception& error) {
        spdlog::error("Failed to stop Prophesee event camera '{}': {}", get_config().name, error.what());
        return false;
    }
}

void PropheseeEventCamera::_capture_loop() {
    // CD events arrive on a thread managed internally by the Metavision SDK (see
    // _on_cd_events()); this loop just keeps the base class's worker thread alive
    // for the lifetime of acquisition.
    constexpr auto kPollInterval = std::chrono::milliseconds(50);
    while (is_running()) {
        std::this_thread::sleep_for(kPollInterval);
    }
}

void PropheseeEventCamera::_post_stop() noexcept {
    _clock_anchored.store(false);
}

void PropheseeEventCamera::_on_cd_events(const void* begin, const void* end) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    const auto* first = static_cast<const Metavision::EventCD*>(begin);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    const auto* last = static_cast<const Metavision::EventCD*>(end);
    if (first == last) {
        return;
    }

    // The Metavision device clock reports microseconds since the stream started, not
    // wall-clock/steady time. Anchor it onto steady_clock once, on the first batch, so
    // every event ends up in the same clock domain the logger's
    // SteadyClockUnixTimeMapper expects (matching IMU samples and the Pylon fallback
    // path). This does not correct for clock drift between the sensor and the host
    // over a long recording -- only single-point anchoring at stream start.
    if (!_clock_anchored.exchange(true)) {
        _device_start_us = static_cast<int64_t>(first->t);
        _host_start_ns = steady_time_ns();
    }

    const auto count = static_cast<size_t>(last - first);
    _conversion_buffer.resize(count);
    for (size_t index = 0; index < count; ++index) {
        const Metavision::EventCD& event = first[index];
        const int64_t device_delta_us = static_cast<int64_t>(event.t) - _device_start_us;
        EventRecord& record = _conversion_buffer[index];
        record.timestamp_ns = static_cast<int64_t>(_host_start_ns) + (device_delta_us * 1000);
        record.x = static_cast<uint16_t>(event.x);
        record.y = static_cast<uint16_t>(event.y);
        record.polarity = event.p > 0 ? 1U : 0U;
        record.reserved = 0;
    }

    process_events(_conversion_buffer.data(), _conversion_buffer.size());
}

} // namespace core

#endif
