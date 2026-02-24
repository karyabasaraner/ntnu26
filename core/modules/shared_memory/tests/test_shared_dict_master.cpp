#include <gtest/gtest.h>
#include <string>

#include "../shared_dict_master.hpp"

const std::string TEST_CONFIG_PATH = "ci/configs/four-cameras.yaml";

TEST(SharedDictMasterTest, InitializeSharedDictMaster) {
    // WHEN: SharedDictMaster is initialized with config with 4 cameras
    const core::SharedDictMaster shared_dict_master(TEST_CONFIG_PATH);
}
