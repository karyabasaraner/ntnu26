#include "event_camera_factory.hpp"

#include "configs.hpp"
#include "event_camera.hpp"

#ifdef CORE_ENABLE_METAVISION
#include "prophesee_event_camera.hpp"
#endif

#include <memory>
#include <stdexcept>

namespace core {

std::unique_ptr<EventCamera> create_event_camera(const EventCameraConfig& config) {
    if (config.backend == EventCameraBackend::prophesee) {
#ifdef CORE_ENABLE_METAVISION
        return std::make_unique<PropheseeEventCamera>(config);
#else
        throw std::runtime_error(
            "Event camera '" + config.name + "' requests the Prophesee backend, but core was built without ENABLE_METAVISION=ON"
        );
#endif
    }
    throw std::runtime_error("Event camera '" + config.name + "' has no resolved backend");
}

} // namespace core
