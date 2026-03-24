#include <gtest/gtest.h>
#include <chrono>
#include <string>
#include <thread>

#include "../../shared_memory/client/reader.hpp"
#include "../../shared_memory/master.hpp"
#include "../../shared_memory/utils.hpp"
#include "../camera_module.hpp"

const std::string TEST_CONFIG_PATH = "ci/configs/ci-four-cameras.yaml";

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
    camera_module.start_cameras();

    // WHEN: A separate reader is initialized for one of the cameras
    std::string const name = "right";
    core::SharedDictReader shared_dict_reader(name);

    // THEN: Reader should be ready to read frames from shared memory
    EXPECT_TRUE(shared_dict_reader.is_ready());

    // WHEN: Reader attempts to read frames
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    core::DataEntry entry;
    shared_dict_reader.read_latest(entry);
    EXPECT_GT(entry.timestamp_ns, 0);
    EXPECT_GE(entry.sequence, 0);
}
