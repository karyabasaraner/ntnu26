#include <gtest/gtest.h>

#include "../../tests/test_config_helpers.hpp"
#include "../configs.hpp"

#include <exception>
#include <stdexcept>
#include <vector>

namespace {

TEST(CoreConfigsTest, LoadParsesNestedCameraImuAndSharedMemoryConfig) {
    // GIVEN: A config file with camera, IMU, and shared-memory sections
    const core::test::TempConfigFile config_file(R"(
cameras:
  - name: front
    fps: 30
    subsample_factor: 2
    v4l2:
      device: /dev/video0
      format: UYVY
      req_buffer_count: 4
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
    EXPECT_EQ(root_config.cameras[0].backend, core::CameraBackend::v4l2);
    EXPECT_EQ(root_config.cameras[0].subsample_factor, 2U);
    ASSERT_EQ(root_config.cameras[0].v4l2.settings.size(), 1);
    EXPECT_EQ(root_config.cameras[0].v4l2.settings[0].id, 0x009a092aU);
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

TEST(CoreConfigsTest, LoadParsesPylonGigECameraFields) {
    // GIVEN: A config file declares a Pylon GigE camera
    const core::test::TempConfigFile config_file(R"(
cameras:
  - name: basler
    fps: 30
    pylon:
      ip_address: 192.168.10.2
      format: BayerRG8
      exposure_time_us: 2000
      gain: 1.5
      packet_size: 4000
      inter_packet_delay: 1000
    writer:
      width: 720
      height: 540
      transforms: []
imus: []
shared_memory:
  - name: basler
    size_per_frame: 1166400
    num_frames: 30
)");

    // WHEN: The config is loaded
    core::Config config;
    config.load(config_file.path());
    const core::CameraConfig& camera = config.get_config().cameras.front();

    // THEN: Backend-specific fields are available
    EXPECT_EQ(camera.backend, core::CameraBackend::pylon);
    EXPECT_EQ(camera.pylon.ip_address, "192.168.10.2");
    EXPECT_DOUBLE_EQ(camera.pylon.exposure_time_us, 2000.0);
    EXPECT_DOUBLE_EQ(camera.pylon.gain, 1.5);
    EXPECT_EQ(camera.pylon.packet_size, 4000U);
    EXPECT_EQ(camera.pylon.inter_packet_delay, 1000U);
}

TEST(CoreConfigsTest, LoadRejectsPylonCameraWithoutIpAddress) {
    // GIVEN: A Pylon camera config has no IP address
    const core::test::TempConfigFile config_file(R"(
cameras:
  - name: basler
    fps: 30
    pylon:
      format: BayerRG8
    writer:
      width: 720
      height: 540
      transforms: []
imus: []
shared_memory: []
)");

    // WHEN: The config is loaded
    core::Config config;

    // THEN: Validation rejects the missing backend-specific field
    EXPECT_THROW(config.load(config_file.path()), std::exception);
}

TEST(CoreConfigsTest, LoadRejectsMultipleCameraBackends) {
    // GIVEN: A camera declares both supported backend mappings
    const core::test::TempConfigFile config_file(R"(
cameras:
  - name: ambiguous
    fps: 30
    v4l2:
      device: /dev/video0
      format: UYVY
      req_buffer_count: 4
      settings: []
    pylon:
      ip_address: 192.168.10.2
      format: BayerRG8
    writer:
      width: 720
      height: 540
      transforms: []
imus: []
shared_memory: []
)");

    // WHEN: The config is loaded
    core::Config config;

    // THEN: Backend selection is rejected as ambiguous
    EXPECT_THROW(config.load(config_file.path()), std::runtime_error);
}

TEST(CoreConfigsTest, LoadParsesFourCameraPylonExample) {
    // GIVEN: The repository's Basler dart example
    core::Config config;

    // WHEN: The example is loaded
    config.load("configs/basler-dart.yaml");
    const std::vector<core::CameraConfig>& cameras = config.get_config().cameras;

    // THEN: Four named Pylon cameras have sequential GigE addresses
    ASSERT_EQ(cameras.size(), 4U);
    EXPECT_EQ(cameras[0].name, "front_left");
    EXPECT_EQ(cameras[0].pylon.ip_address, "192.168.10.2");
    EXPECT_EQ(cameras[1].name, "front_right");
    EXPECT_EQ(cameras[1].pylon.ip_address, "192.168.10.3");
    EXPECT_EQ(cameras[2].name, "left");
    EXPECT_EQ(cameras[2].pylon.ip_address, "192.168.10.4");
    EXPECT_EQ(cameras[3].name, "right");
    EXPECT_EQ(cameras[3].pylon.ip_address, "192.168.10.5");
}

TEST(CoreConfigsTest, LoadAppliesDocumentedDefaultsForOptionalFields) {
    // GIVEN: A config file omits optional camera fields
    const core::test::TempConfigFile config_file(R"(
cameras:
  - name: right
    fps: 30
    v4l2:
      device: /dev/video3
      format: UYVY
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
    fps: 30
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
