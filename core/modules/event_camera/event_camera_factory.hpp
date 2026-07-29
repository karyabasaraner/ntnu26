#ifndef WORKSPACES_CORE_CORE_MODULES_EVENT_CAMERA_EVENT_CAMERA_FACTORY_HPP
#define WORKSPACES_CORE_CORE_MODULES_EVENT_CAMERA_EVENT_CAMERA_FACTORY_HPP

#include "event_camera.hpp"
#include "configs.hpp"

#include <memory>

namespace core {

std::unique_ptr<EventCamera> create_event_camera(const EventCameraConfig& config);

} // namespace core

#endif
