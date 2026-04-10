#include <gtest/gtest.h>

#include "../../../tests/test_async_helpers.hpp"
#include "../client/client.hpp"
#include "../client/writer.hpp"
#include "../logger/logger.hpp"
#include "../master.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <mcap/reader.hpp>
#include <string>
#include <system_error>
#include <vector>

namespace {

constexpr char kTestConfigPath[] = "ci/configs/ci-four-cameras.yaml";

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

template <typename T>
T read_value(const std::byte* data, std::size_t offset) {
    T value{};
    std::memcpy(&value, data + offset, sizeof(T));
    return value;
}

struct DecodedImuMessage {
    std::string topic;
    uint64_t timestamp_ns{0};
    uint32_t sequence{0};
    float x{0.0F};
    float y{0.0F};
    float z{0.0F};
};

std::vector<DecodedImuMessage> read_imu_messages(const std::string& mcap_path) {
    mcap::McapReader reader;
    const auto open_status = reader.open(mcap_path);
    EXPECT_TRUE(open_status.ok()) << open_status.message;
    if (!open_status.ok()) {
        return {};
    }

    const auto summary_status = reader.readSummary(mcap::ReadSummaryMethod::AllowFallbackScan);
    EXPECT_TRUE(summary_status.ok()) << summary_status.message;
    if (!summary_status.ok()) {
        return {};
    }

    std::vector<DecodedImuMessage> messages;
    for (const auto& view : reader.readMessages()) {
        if (view.channel == nullptr || view.channel->topic.rfind("/imu/", 0) != 0) {
            continue;
        }

        EXPECT_NE(view.message.data, nullptr);
        EXPECT_EQ(view.message.dataSize, sizeof(uint64_t) + sizeof(uint32_t) + 3 * sizeof(float));
        if (view.message.data == nullptr ||
            view.message.dataSize != sizeof(uint64_t) + sizeof(uint32_t) + 3 * sizeof(float)) {
            continue;
        }

        const auto* payload = view.message.data;
        constexpr std::size_t timestamp_offset = 0;
        constexpr std::size_t sequence_offset = sizeof(uint64_t);
        constexpr std::size_t x_offset = sequence_offset + sizeof(uint32_t);
        constexpr std::size_t y_offset = x_offset + sizeof(float);
        constexpr std::size_t z_offset = y_offset + sizeof(float);

        messages.push_back(DecodedImuMessage{
            .topic = view.channel->topic,
            .timestamp_ns = read_value<uint64_t>(payload, timestamp_offset),
            .sequence = read_value<uint32_t>(payload, sequence_offset),
            .x = read_value<float>(payload, x_offset),
            .y = read_value<float>(payload, y_offset),
            .z = read_value<float>(payload, z_offset),
        });
    }

    return messages;
}

} // namespace

namespace core {

class SharedDictLoggerTestPeer {
public:
    static std::vector<SensorStream>& sensor_streams(SharedDictLogger& logger) {
        return logger._sensor_streams;
    }

    static void process_sensor_stream(SharedDictLogger& logger, SensorStream& stream) {
        logger._process_sensor_stream(stream);
    }
};

} // namespace core

TEST(LoggerTest, ProcessSensorStreamWritesImuSampleToMcap) {
    TempMcapPath output_path;
    const core::SharedDictMaster shared_dict_master(kTestConfigPath);
    core::WriterConfig writer_config;
    core::SharedDictWriter shared_dict_writer("accelerometer", writer_config);
    const core::SharedDictClient shared_dict_client("accelerometer");

    ASSERT_TRUE(shared_dict_client.is_ready());

    {
        core::SharedDictLogger logger(kTestConfigPath, output_path.string());

        auto& streams = core::SharedDictLoggerTestPeer::sensor_streams(logger);
        auto stream_it = std::find_if(streams.begin(), streams.end(), [](const core::SensorStream& stream) {
            return stream.name == "accelerometer";
        });
        ASSERT_NE(stream_it, streams.end());
        ASSERT_NE(stream_it->reader, nullptr);
        ASSERT_TRUE(stream_it->reader->is_ready());
        EXPECT_NE(stream_it->channel_id, 0U);

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

        core::SharedDictLoggerTestPeer::process_sensor_stream(logger, *stream_it);
    }

    const auto messages = read_imu_messages(output_path.string());
    ASSERT_EQ(messages.size(), 1U);
    EXPECT_EQ(messages[0].topic, "/imu/accelerometer");
    EXPECT_EQ(messages[0].timestamp_ns, 424242U);
    EXPECT_EQ(messages[0].sequence, 41U);
    EXPECT_FLOAT_EQ(messages[0].x, 1.25F);
    EXPECT_FLOAT_EQ(messages[0].y, -2.5F);
    EXPECT_FLOAT_EQ(messages[0].z, 3.75F);
}

TEST(LoggerTest, ProcessSensorStreamWithUnavailableSharedMemoryWritesNoMessages) {
    TempMcapPath output_path;

    {
        core::SharedDictLogger logger(kTestConfigPath, output_path.string());

        auto& streams = core::SharedDictLoggerTestPeer::sensor_streams(logger);
        auto stream_it = std::find_if(streams.begin(), streams.end(), [](const core::SensorStream& stream) {
            return stream.name == "accelerometer";
        });
        ASSERT_NE(stream_it, streams.end());
        ASSERT_NE(stream_it->reader, nullptr);
        EXPECT_FALSE(stream_it->reader->is_ready());

        core::SharedDictLoggerTestPeer::process_sensor_stream(logger, *stream_it);
    }

    const auto messages = read_imu_messages(output_path.string());
    EXPECT_TRUE(messages.empty());
}
