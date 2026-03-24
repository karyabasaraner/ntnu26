#include <gtest/gtest.h>
#include <string>

#include "../client.hpp"
#include "../master.hpp"
#include "configs.hpp"

const std::string TEST_CONFIG_PATH = "ci/configs/ci-four-cameras.yaml";

TEST(SharedDictTest, InitializeSharedDictMaster) {
    // WHEN: SharedDictMaster is initialized with config with 4 cameras
    const core::SharedDictMaster shared_dict_master(TEST_CONFIG_PATH);

    // THEN: SharedDictMaster should be initialized successfully and ready to use
    EXPECT_TRUE(shared_dict_master.is_initialized());
}

TEST(ShareDictTest, IntializeShareDictNoMaster) {
    // GIVEN: SharedDictClient without master is not ready
    const std::string name = "right";

    // WHEN: SharedDictClient is initialized with config for a camera
    core::SharedDictClient const shared_dict_client(name);

    // THEN: SharedDictClient should not be ready to write
    EXPECT_FALSE(shared_dict_client.is_ready());
}

TEST(ShareDictTest, InitializeSharedDictClientWithMaster) {
    // GIVEN: A shm master that is setup
    const core::SharedDictMaster shared_dict_master(TEST_CONFIG_PATH);

    // WHEN: SharedDictClient is initialized with config for a camera
    const std::string name = "right";
    core::SharedDictClient const shared_dict_client(name);

    // THEN: SharedDictClient should be ready to write
    EXPECT_TRUE(shared_dict_client.is_ready());
}
