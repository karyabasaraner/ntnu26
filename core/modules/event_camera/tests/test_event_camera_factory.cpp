#include "../event_camera_factory.hpp"
#include "configs.hpp"

#include <stdexcept>
#include <string>

#include <gtest/gtest.h>

namespace {

std::string factory_error(const core::EventCameraConfig& config) {
    try {
        static_cast<void>(core::create_event_camera(config));
    } catch (const std::runtime_error& error) {
        return error.what();
    }
    return {};
}

#ifndef CORE_ENABLE_METAVISION
TEST(EventCameraFactoryTest, RejectsPropheseeCameraWhenBackendIsNotCompiled) {
    // GIVEN: A valid Prophesee event camera configuration in a default build
    core::EventCameraConfig config{};
    config.backend = core::EventCameraBackend::prophesee;
    config.name = "event_left";

    // WHEN: The factory is asked to create the event camera
    // THEN: It explains how to enable the optional backend
    const std::string error = factory_error(config);

    EXPECT_NE(error.find("ENABLE_METAVISION=ON"), std::string::npos);
}
#endif

TEST(EventCameraFactoryTest, RejectsUnresolvedBackend) {
    // GIVEN: An event camera configuration has no resolved backend
    core::EventCameraConfig config{};
    config.name = "event_left";

    // WHEN: The factory is asked to create the event camera
    // THEN: It rejects the unsupported backend
    EXPECT_FALSE(factory_error(config).empty());
}

} // namespace
