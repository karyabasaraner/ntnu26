#include <gtest/gtest.h>
#include <string>

#include "../camera_module.hpp"

const std::string TEST_CONFIG_PATH = "ci/configs/four-cameras.yaml";

TEST(CameraModuleTest, InitializeCameras) {
    // WHEN: Camera module is initialized with config with 4 cameras
    core::CameraModule camera_module(TEST_CONFIG_PATH);

    // THEN: 4 cameras are initialized
    EXPECT_EQ(camera_module.get_num_cameras(), 4);
}

TEST(CameraModuleTest, StartStopCameras) {
    // GIVEN: Camera module is initialized with config with 4 cameras
    core::CameraModule camera_module(TEST_CONFIG_PATH);

    // WHEN: All cameras are started
    camera_module.start_cameras();

    // THEN: All cameras are running
    EXPECT_EQ(camera_module.get_running_cameras(), 4);
    
    // WHEN: All cameras are stopped
    camera_module.stop_cameras();

    // THEN: No cameras are running
    EXPECT_EQ(camera_module.get_running_cameras(), 0);
}
