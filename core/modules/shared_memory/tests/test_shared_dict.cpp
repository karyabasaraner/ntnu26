#include <gtest/gtest.h>
#include <string>

#include "../shared_dict_client.hpp"
#include "../shared_dict_master.hpp"
#include "configs.hpp"

const std::string TEST_CONFIG_PATH = "ci/configs/four-cameras.yaml";

TEST(SharedDictTest, InitializeSharedDictMaster) {
    // WHEN: SharedDictMaster is initialized with config with 4 cameras
    const core::SharedDictMaster shared_dict_master(TEST_CONFIG_PATH);

    // THEN: SharedDictMaster should be initialized successfully and ready to use
    EXPECT_TRUE(shared_dict_master.is_initialized());
}

TEST(ShareDictTest, IntializeShareDictNoMaster) {
    // GIVEN: SharedDictClient without master is not ready
    core::CameraConfig const config = {
        .name = "right",
    };

    // WHEN: SharedDictClient is initialized with config for a camera
    core::SharedDictClient const shared_dict_client(config);

    // THEN: SharedDictClient should not be ready to write
    EXPECT_FALSE(shared_dict_client.is_ready());
}

TEST(ShareDictTest, InitializeSharedDictClientWithMaster) {
    // GIVEN: A shm master that is setup
    const core::SharedDictMaster shared_dict_master(TEST_CONFIG_PATH);

    // WHEN: SharedDictClient is initialized with config for a camera
    core::CameraConfig const config = {
        .name = "right",
    };
    core::SharedDictClient const shared_dict_client(config);

    // THEN: SharedDictClient should be ready to write
    EXPECT_TRUE(shared_dict_client.is_ready());
}
