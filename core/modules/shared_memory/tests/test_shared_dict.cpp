#include <gtest/gtest.h>
#include <string>

#include "../shared_dict_master.hpp"
#include "../shared_dict_client.hpp"

const std::string TEST_CONFIG_PATH = "ci/configs/four-cameras.yaml";

TEST(SharedDictTest, InitializeSharedDictMaster) {
    // WHEN: SharedDictMaster is initialized with config with 4 cameras
    const core::SharedDictMaster shared_dict_master(TEST_CONFIG_PATH);

    // THEN: SharedDictMaster should be initialized successfully and ready to use
    EXPECT_TRUE(shared_dict_master.is_initialized());
}

TEST(ShareDictTest, IntializeShareDictClientNoMaster) {
    // GIVEN: SharedDictClient without master is not ready
    core::SharedDictClient shared_dict_client;

    // WHEN: SharedDictClient is initialized with config for a camera
    core::CameraConfig config = {
        .name = "front_left",
    };
    shared_dict_client.initialize(config);

    // THEN: SharedDictClient should not be ready to write
    EXPECT_FALSE(shared_dict_client.is_ready());
}

TEST(ShareDictTest, IntializeShareDictClientMaster) {
    // WHEN: SharedDictMaster is initialized with config with 4 cameras
    const core::SharedDictMaster shared_dict_master(TEST_CONFIG_PATH);

    // AND: SharedDictClient
    core::SharedDictClient shared_dict_client;

    // WHEN: SharedDictClient is initialized with config for a camera
    core::CameraConfig config = {
        .name = "front_left",
    };
    shared_dict_client.initialize(config);

    // THEN: SharedDictClient should not be ready to write
    EXPECT_TRUE(shared_dict_client.is_ready());
}
