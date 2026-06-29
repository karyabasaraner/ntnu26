#ifndef WORKSPACES_CORE_CORE_MODULES_CAMERA_CAMERA_FACTORY_HPP
#define WORKSPACES_CORE_CORE_MODULES_CAMERA_CAMERA_FACTORY_HPP

#include "base_camera.hpp"
#include "configs.hpp"

#include <memory>

namespace core {

std::unique_ptr<Camera> create_camera(const CameraConfig& config);

} // namespace core

#endif
