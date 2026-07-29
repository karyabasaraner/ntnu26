#include "../event_camera_module.hpp"

#include <string>

#include <gtest/gtest.h>

namespace {

const std::string TEST_CONFIG_PATH = "ci/configs/ci-event-cameras.yaml";

TEST(EventCameraModuleTest, InitializeEventCameras) {
    // GIVEN: A config declares 1 event camera

    // WHEN: Event camera module is initialized
    const core::EventCameraModule event_camera_module(TEST_CONFIG_PATH);

    // THEN: 1 event camera slot is accounted for
    EXPECT_EQ(event_camera_module.get_num_event_cameras(), 1);
}

#ifndef CORE_ENABLE_METAVISION
TEST(EventCameraModuleTest, SurvivesUncompiledBackendWithoutCrashing) {
    // GIVEN: A default build without the Metavision SDK, and a config declaring a
    // Prophesee event camera

    // WHEN: The module is initialized and started
    core::EventCameraModule event_camera_module(TEST_CONFIG_PATH);
    event_camera_module.start_event_cameras();

    // THEN: The module still reports the declared camera, but none are running --
    // and critically, construction/start did not throw or crash the process
    EXPECT_EQ(event_camera_module.get_num_event_cameras(), 1);
    EXPECT_EQ(event_camera_module.get_running_event_cameras(), 0);

    event_camera_module.stop_event_cameras();
}
#endif

} // namespace
