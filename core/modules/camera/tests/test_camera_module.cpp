#include <gtest/gtest.h>

#include "../camera_module.hpp"

TEST(CameraModuleTest, LoadConfig) {
    const std::string config_path = "configs/core-0.yaml";
    core::CameraModule camera_module(config_path);

    // TODO(MJ): Expand this test to check that the config is loaded correctly and that cameras are initialized as expected.
}
