#ifndef WORKSPACES_CORE_CORE_MODULES_EVENT_CAMERA_EVENT_CAMERA_MODULE_HPP
#define WORKSPACES_CORE_CORE_MODULES_EVENT_CAMERA_EVENT_CAMERA_MODULE_HPP

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "../utils/configs.hpp"
#include "event_camera.hpp"

namespace core {

class EventCameraModule {
/*
    Event camera module reads config, initializes event cameras.
*/
public:
    explicit EventCameraModule(const std::string& config_path);

    uint8_t get_num_event_cameras() const;
    uint8_t get_running_event_cameras() const;
    void start_event_cameras(size_t index = -1);
    void stop_event_cameras(size_t index = -1);

private:
    Config _config;
    std::vector<std::unique_ptr<EventCamera>> _event_cameras;

    void _initialize_event_cameras();
};

} // namespace core
#endif
