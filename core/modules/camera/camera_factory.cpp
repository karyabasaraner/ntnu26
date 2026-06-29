#include "camera_factory.hpp"

#include "base_camera.hpp"
#include "configs.hpp"
#include "v4l2_camera.hpp"

#ifdef CORE_ENABLE_PYLON
#include "pylon_camera.hpp"
#endif

#include <memory>
#include <stdexcept>

namespace core {

std::unique_ptr<Camera> create_camera(const CameraConfig& config) {
    if (config.backend == CameraBackend::v4l2) {
        return std::make_unique<V4L2Camera>(config);
    }
    if (config.backend == CameraBackend::pylon) {
#ifdef CORE_ENABLE_PYLON
        return std::make_unique<PylonCamera>(config);
#else
        throw std::runtime_error(
            "Camera '" + config.name + "' requests the Pylon backend, but core was built without ENABLE_PYLON=ON"
        );
#endif
    }
    throw std::runtime_error("Camera '" + config.name + "' has no resolved backend");
}

} // namespace core
