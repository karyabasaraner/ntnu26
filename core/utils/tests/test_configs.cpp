#include <gtest/gtest.h>

#include "../../tests/test_config_helpers.hpp"
#include "../configs.hpp"

#include <exception>

namespace {

TEST(CoreConfigsTest, LoadParsesNestedCameraImuAndSharedMemoryConfig) {
    // GIVEN: A config file with camera, IMU, and shared-memory sections
    const core::test::TempConfigFile config_file(R"(
cameras:
  - name: front
    device: /dev/video0
    format: UYVY
    fps: 30
    req_buffer_count: 4
    subsample_factor: 2
    settings:
      - name: frame_sync
        value: 1
        id: 0x009a092a
    writer:
      width: 1280
      height: 720
      transforms:
        - name: UYVY2RGB
imus:
  - name: accelerometer
    device: iio:device0
    sampling_frequency: 400
    scale: 0.5
    buffer_samples: 128
    watermark_samples: 32
    channels:
      - accel_x
      - accel_y
      - accel_z
    writer:
      width: 3
      height: 1
      transforms: []
shared_memory:
  - name: front
    size_per_frame: 2764800
    num_frames: 300
  - name: accelerometer
    size_per_frame: 12
    num_frames: 4000
)");

    // WHEN: The config file is loaded
    core::Config config;
    config.load(config_file.path());
    const core::RootConfig& root_config = config.get_config();

    // THEN: Camera fields are parsed from the nested config
    ASSERT_EQ(root_config.cameras.size(), 1);
    EXPECT_EQ(root_config.cameras[0].name, "front");
    EXPECT_EQ(root_config.cameras[0].subsample_factor, 2U);
    ASSERT_EQ(root_config.cameras[0].settings.size(), 1);
    EXPECT_EQ(root_config.cameras[0].settings[0].id, 0x009a092aU);
    ASSERT_EQ(root_config.cameras[0].writer.transforms.size(), 1);
    EXPECT_EQ(root_config.cameras[0].writer.transforms[0].name, "UYVY2RGB");

    // THEN: IMU fields are parsed from the nested config
    ASSERT_EQ(root_config.imus.size(), 1);
    EXPECT_EQ(root_config.imus[0].name, "accelerometer");
    EXPECT_FLOAT_EQ(root_config.imus[0].sampling_frequency, 400.0F);
    EXPECT_EQ(root_config.imus[0].channels.size(), 3);

    // THEN: Shared-memory fields are parsed from the nested config
    ASSERT_EQ(root_config.shared_memory.size(), 2);
    EXPECT_EQ(root_config.shared_memory[1].name, "accelerometer");
    EXPECT_EQ(root_config.shared_memory[1].size_per_frame, 12U);
}

TEST(CoreConfigsTest, LoadAppliesDocumentedDefaultsForOptionalFields) {
    // GIVEN: A config file omits optional camera fields
    const core::test::TempConfigFile config_file(R"(
cameras:
  - name: right
    device: /dev/video3
    format: UYVY
    fps: 30
    req_buffer_count: 2
    settings: []
    writer:
      width: 640
      height: 480
      transforms: []
imus: []
shared_memory:
  - name: right
    size_per_frame: 921600
    num_frames: 32
)");

    // WHEN: The config file is loaded
    core::Config config;
    config.load(config_file.path());
    const core::RootConfig& root_config = config.get_config();

    // THEN: Optional fields use documented defaults
    ASSERT_EQ(root_config.cameras.size(), 1);
    EXPECT_EQ(root_config.cameras[0].subsample_factor, 1U);
    EXPECT_TRUE(root_config.cameras[0].writer.transforms.empty());
}

TEST(CoreConfigsTest, LoadThrowsWhenRequiredFieldsAreMissing) {
    // GIVEN: A config file is missing required camera fields
    const core::test::TempConfigFile config_file(R"(
cameras:
  - name: broken
    format: UYVY
    fps: 30
    req_buffer_count: 1
    settings: []
    writer:
      width: 640
      height: 480
      transforms: []
imus: []
shared_memory: []
)");

    // WHEN: The config file is loaded
    core::Config config;

    // THEN: Loading fails with an exception
    EXPECT_THROW(config.load(config_file.path()), std::exception);
}

} // namespace
