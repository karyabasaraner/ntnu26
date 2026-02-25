#include <gtest/gtest.h>
#include <string>

#include "../../shared_memory/shared_dict_master.hpp"
#include "../camera_module.hpp"
#include "/workspaces/core/core/modules/shared_memory/shared_dict_client.hpp"
#include "configs.hpp"

const std::string TEST_CONFIG_PATH = "ci/configs/four-cameras.yaml";

TEST(CameraModuleTest, InitializeCameras) {
    // WHEN: Camera module is initialized with config with 4 cameras
    const core::CameraModule camera_module(TEST_CONFIG_PATH);

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

TEST(CameraModuleTest, StartWithWritingAndReading) {
    // GIVEN: A shm master that is setup
    const core::SharedDictMaster shared_dict_master(TEST_CONFIG_PATH);

    // AND: Camera module is initialized with config with 4 cameras
    core::CameraModule camera_module(TEST_CONFIG_PATH);


    // AND: A separate reading client reading the right camera
    core::CameraConfig const config = {
        .name = "right",
    };
    core::SharedDictClient const shared_dict_client(config);


    camera_module.start_cameras();

    // THEN: The current head has advanced, indicating that frames are being written to shared memory

}
