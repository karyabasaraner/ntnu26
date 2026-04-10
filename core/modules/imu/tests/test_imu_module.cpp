#include <gtest/gtest.h>
#include <string>

#include "../imu_module.hpp"

const std::string TEST_CONFIG_PATH = "ci/configs/ci-four-cameras.yaml";

TEST(IMUModuleTest, InitializeIMUs) {
    // GIVEN: A config declares 2 IMUs

    // WHEN: IMU module is initialized with config with 2 IMUs
    const core::IMUModule imu_module(TEST_CONFIG_PATH);

    // THEN: 2 IMUs are initialized
    EXPECT_EQ(imu_module.get_num_imus(), 2);
}
