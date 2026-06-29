#include "../camera_factory.hpp"
#include "configs.hpp"

#include <stdexcept>
#include <string>

#include <gtest/gtest.h>

namespace {

std::string factory_error(const core::CameraConfig& config) {
    try {
        static_cast<void>(core::create_camera(config));
    } catch (const std::runtime_error& error) {
        return error.what();
    }
    return {};
}

#ifndef CORE_ENABLE_PYLON
TEST(CameraFactoryTest, RejectsPylonCameraWhenBackendIsNotCompiled) {
    // GIVEN: A valid Pylon camera configuration in a default build
    core::CameraConfig config{};
    config.backend = core::CameraBackend::pylon;
    config.name = "basler";
    config.pylon.ip_address = "192.168.10.2";

    // WHEN: The factory is asked to create the camera
    // THEN: It explains how to enable the optional backend
    const std::string error = factory_error(config);

    EXPECT_NE(error.find("ENABLE_PYLON=ON"), std::string::npos);
}
#endif

TEST(CameraFactoryTest, RejectsUnresolvedBackend) {
    // GIVEN: A camera configuration has no resolved backend
    core::CameraConfig config{};
    config.name = "camera";

    // WHEN: The factory is asked to create the camera
    // THEN: It rejects the unsupported backend
    EXPECT_FALSE(factory_error(config).empty());
}

} // namespace
