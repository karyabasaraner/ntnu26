#include <gtest/gtest.h>
#include <string>

#include "../camera_module.hpp"

TEST(CameraModuleTest, InitializeCameras) {
    const std::string config_path = "ci/configs/four-cameras.yaml";
    core::CameraModule camera_module(config_path);
    EXPECT_EQ(camera_module.get_num_cameras(), 4);
}
