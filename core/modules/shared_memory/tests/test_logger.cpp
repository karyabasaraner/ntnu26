#include <gtest/gtest.h>

#include "../../../tests/test_async_helpers.hpp"
#include "../client/client.hpp"
#include "../client/writer.hpp"
#include "../logger/log_reader.hpp"
#include "../logger/logger.hpp"
#include "../logger/steady_clock_unix_time_mapper.hpp"
#include "../master.hpp"
#include "../ringbuffer.hpp"
#include "configs.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <filesystem>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

namespace {

const std::string kTestConfigPath = "ci/configs/ci-four-cameras.yaml";
constexpr int64_t kTestSteadyToUnixOffsetNs = 1'700'000'000'000'000'000LL;

class TempMcapPath {
public:
    TempMcapPath() :
        _path(std::filesystem::temp_directory_path() /
              std::filesystem::path("core-logger-test-" + std::to_string(_counter++) + ".mcap")) {}

    ~TempMcapPath() {
        std::error_code error;
        std::filesystem::remove(_path, error);
    }

    TempMcapPath(const TempMcapPath&) = delete;
    TempMcapPath& operator=(const TempMcapPath&) = delete;
    TempMcapPath(TempMcapPath&&) = delete;
    TempMcapPath& operator=(TempMcapPath&&) = delete;

    [[nodiscard]] std::string string() const {
        return _path.string();
    }

private:
    inline static std::size_t _counter{0};
    std::filesystem::path _path;
};

class TempConfigPath {
public:
    TempConfigPath() :
        _path(std::filesystem::temp_directory_path() /
              std::filesystem::path("core-logger-config-" + std::to_string(_counter++) + ".yaml")) {}

    ~TempConfigPath() {
        std::error_code error;
        std::filesystem::remove(_path, error);
    }

    TempConfigPath(const TempConfigPath&) = delete;
    TempConfigPath& operator=(const TempConfigPath&) = delete;
    TempConfigPath(TempConfigPath&&) = delete;
    TempConfigPath& operator=(TempConfigPath&&) = delete;

    [[nodiscard]] std::string string() const {
        return _path.string();
    }

    void write_text(const std::string& content) const {
        std::ofstream out(_path);
        out << content;
    }

private:
    inline static std::size_t _counter{0};
    std::filesystem::path _path;
};

struct LoggedImuMessage {
    uint64_t timestamp_ns{0};
    uint32_t sequence{0};
    float x{0.0F};
    float y{0.0F};
    float z{0.0F};
};

std::vector<LoggedImuMessage> read_imu_messages(const std::string& mcap_path) {
    const auto log_file = core::read_log_file(mcap_path);
    std::vector<LoggedImuMessage> messages;
    const auto* imu = log_file.get_imu_data("/imu/accelerometer");
    if (imu == nullptr) {
        return messages;
    }

    for (std::size_t index = 0; index < imu->timestamp_ns.size(); ++index) {
        messages.push_back(LoggedImuMessage{
            .timestamp_ns = imu->timestamp_ns[index],
            .sequence = imu->sequence[index],
            .x = imu->x[index],
            .y = imu->y[index],
            .z = imu->z[index],
        });
    }

    return messages;
}

void run_logger_until_stopped(core::SharedDictLogger& logger, std::thread& logger_thread) {
    logger_thread = std::thread([&logger]() {
        logger.run();
    });
}

} // namespace

TEST(LoggerTest, SteadyClockUnixTimeMapperPreservesTimestampDurations) {
    // GIVEN: A deterministic steady-to-Unix timestamp mapper
    const core::SteadyClockUnixTimeMapper mapper(kTestSteadyToUnixOffsetNs);

    // WHEN: Two steady-clock timestamps are mapped into Unix time
    const auto first_timestamp = mapper.to_unix_time_ns(1'000U);
    const auto second_timestamp = mapper.to_unix_time_ns(2'500U);

    // THEN: The absolute timestamps are shifted while their interval remains unchanged
    EXPECT_EQ(first_timestamp, 1'700'000'000'000'001'000ULL);
    EXPECT_EQ(second_timestamp, 1'700'000'000'000'002'500ULL);
    EXPECT_EQ(second_timestamp - first_timestamp, 1'500U);
}

TEST(LoggerTest, LoggerWritesImuSampleToMcap) {
    // GIVEN: Shared memory, an accelerometer writer, and the logger are ready
    TempMcapPath const output_path;
    {
        const core::SharedDictMaster shared_dict_master(kTestConfigPath);
        static_cast<void>(shared_dict_master);
        core::WriterConfig const writer_config;
        core::SharedDictWriter shared_dict_writer("accelerometer", writer_config);
        const core::SharedDictClient shared_dict_client("accelerometer");
        ASSERT_TRUE(shared_dict_client.is_ready());

        core::SharedDictLogger logger(kTestConfigPath, output_path.string());
        std::thread logger_thread;
        run_logger_until_stopped(logger, logger_thread);

        // WHEN: An IMU sample is published and the logger has time to consume it
        const std::array<float, 3> imu_sample{1.25F, -2.5F, 3.75F};
        shared_dict_writer.add(
            "accelerometer",
            imu_sample.data(),
            sizeof(imu_sample),
            41U,
            424242U);

        core::Buffer* const buffer = shared_dict_client.get_buffer();
        ASSERT_NE(buffer, nullptr);
        ASSERT_TRUE(core::test::wait_for_predicate([buffer] {
            return buffer->sequence.load(std::memory_order_acquire) == 41U;
        }));

        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        logger.request_stop();
        logger_thread.join();
    }

    // THEN: The logger writes one IMU message with the sample payload and timestamp
    const auto messages = read_imu_messages(output_path.string());
    ASSERT_EQ(messages.size(), 1U);
    EXPECT_GT(messages[0].timestamp_ns, 0ULL);
    EXPECT_EQ(messages[0].sequence, 41U);
    EXPECT_FLOAT_EQ(messages[0].x, 1.25F);
    EXPECT_FLOAT_EQ(messages[0].y, -2.5F);
    EXPECT_FLOAT_EQ(messages[0].z, 3.75F);

    const auto log_file = core::read_log_file(output_path.string());
    const auto* imu = log_file.get_imu_data("/imu/accelerometer");
    ASSERT_NE(imu, nullptr);
    ASSERT_EQ(imu->timestamp_ns.size(), 1U);
    ASSERT_EQ(imu->sequence.size(), 1U);
}

TEST(LoggerTest, LoggerSkipsInvalidImuSampleWithoutWritingMessages) {
    // GIVEN: Shared memory, an accelerometer writer, and the logger are ready
    TempMcapPath const output_path;
    {
        const core::SharedDictMaster shared_dict_master(kTestConfigPath);
        static_cast<void>(shared_dict_master);
        core::WriterConfig const writer_config;
        core::SharedDictWriter shared_dict_writer("accelerometer", writer_config);
        const core::SharedDictClient shared_dict_client("accelerometer");
        ASSERT_TRUE(shared_dict_client.is_ready());

        core::SharedDictLogger logger(kTestConfigPath, output_path.string());
        std::thread logger_thread;
        run_logger_until_stopped(logger, logger_thread);

        // WHEN: An undersized IMU sample is published
        const std::array<float, 2> invalid_sample{1.0F, 2.0F};
        shared_dict_writer.add(
            "accelerometer",
            invalid_sample.data(),
            sizeof(invalid_sample),
            7U,
            123456U);

        core::Buffer* const buffer = shared_dict_client.get_buffer();
        ASSERT_NE(buffer, nullptr);
        ASSERT_TRUE(core::test::wait_for_predicate([buffer] {
            return buffer->sequence.load(std::memory_order_acquire) == 7U;
        }));

        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        logger.request_stop();
        logger_thread.join();
    }

    // THEN: No IMU messages are written
    EXPECT_TRUE(read_imu_messages(output_path.string()).empty());
}

TEST(LoggerTest, LoggerSkipsInvalidCameraSampleWithoutWritingMessages) {
    // GIVEN: Shared memory, a camera writer, and the logger are ready
    TempMcapPath const output_path;
    {
        const core::SharedDictMaster shared_dict_master(kTestConfigPath);
        static_cast<void>(shared_dict_master);
        core::WriterConfig camera_writer_config;
        camera_writer_config.width = 1280U;
        camera_writer_config.height = 720U;
        core::SharedDictWriter shared_dict_writer("front_left", camera_writer_config);
        const core::SharedDictClient shared_dict_client("front_left");
        ASSERT_TRUE(shared_dict_client.is_ready());

        core::SharedDictLogger logger(kTestConfigPath, output_path.string());
        std::thread logger_thread;
        run_logger_until_stopped(logger, logger_thread);

        // WHEN: An undersized camera sample is published
        const std::array<std::byte, 1> invalid_sample{std::byte{0x01}};
        shared_dict_writer.add(
            "front_left",
            invalid_sample.data(),
            invalid_sample.size(),
            5U,
            987654U);

        core::Buffer* const buffer = shared_dict_client.get_buffer();
        ASSERT_NE(buffer, nullptr);
        ASSERT_TRUE(core::test::wait_for_predicate([buffer] {
            return buffer->sequence.load(std::memory_order_acquire) == 5U;
        }));

        std::this_thread::sleep_for(std::chrono::milliseconds(40));
        logger.request_stop();
        logger_thread.join();
    }

    // THEN: No IMU messages are written and the log remains empty
    const auto log_file = core::read_log_file(output_path.string());
    EXPECT_TRUE(log_file.topics().empty());
}

TEST(LoggerTest, LoggerWritesCameraSampleToMcap) {
    // GIVEN: A minimal camera config, shared memory, and a camera writer
    TempConfigPath const config_path;
    config_path.write_text(
        R"(cameras:
  - name: cam0
    writer:
      width: 1
      height: 1
    device: /dev/video0
    fps: 30
    subsample_factor: 1
    settings: []
    format: UYVY
    req_buffer_count: 1
imus: []
shared_memory:
  - name: cam0
    size_per_frame: 3
    num_frames: 2
)"
    );

    TempMcapPath const output_path;
    {
        const core::SharedDictMaster shared_dict_master(config_path.string());
        static_cast<void>(shared_dict_master);
        core::WriterConfig camera_writer_config;
        camera_writer_config.width = 1U;
        camera_writer_config.height = 1U;
        core::SharedDictWriter shared_dict_writer("cam0", camera_writer_config);
        const core::SharedDictClient shared_dict_client("cam0");
        ASSERT_TRUE(shared_dict_client.is_ready());

        core::SharedDictLogger logger(config_path.string(), output_path.string());
        std::thread logger_thread;
        run_logger_until_stopped(logger, logger_thread);

        // WHEN: A camera sample is published and the logger has time to consume it
        const std::array<std::byte, 3> rgb_sample{std::byte{0x10}, std::byte{0x20}, std::byte{0x30}};
        shared_dict_writer.add(
            "cam0",
            rgb_sample.data(),
            rgb_sample.size(),
            9U,
            654321U);

        core::Buffer* const buffer = shared_dict_client.get_buffer();
        ASSERT_NE(buffer, nullptr);
        ASSERT_TRUE(core::test::wait_for_predicate([buffer] {
            return buffer->sequence.load(std::memory_order_acquire) == 9U;
        }));

        std::this_thread::sleep_for(std::chrono::milliseconds(40));
        logger.request_stop();
        logger_thread.join();
    }

    // THEN: The logger writes a Foxglove-compatible compressed image message
    const auto log_file = core::read_log_file(output_path.string());
    const auto* camera = log_file.get_camera_data("/camera/cam0/image/compressed");
    ASSERT_NE(camera, nullptr);
    ASSERT_EQ(camera->timestamp_ns.size(), 1U);
    ASSERT_EQ(camera->frame_id.size(), 1U);
    ASSERT_EQ(camera->format.size(), 1U);
    ASSERT_EQ(camera->jpeg_data.size(), 1U);
    EXPECT_EQ(camera->frame_id[0], "cam0");
    EXPECT_EQ(camera->format[0], "jpeg");
    EXPECT_FALSE(camera->jpeg_data[0].empty());
}
