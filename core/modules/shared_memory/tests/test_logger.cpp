#include <gtest/gtest.h>

#include "../../../tests/test_async_helpers.hpp"
#include "../client/client.hpp"
#include "../client/reader.hpp"
#include "../client/writer.hpp"
#include "../logger/logger.hpp"
#include "../logger/schema.hpp"
#include "../master.hpp"
#include "../ringbuffer.hpp"
#include "configs.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <mcap/reader.hpp>
#include <mcap/types.hpp>
#include <mcap/writer.hpp>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>
#include <vector>

namespace {

const std::string kTestConfigPath = "ci/configs/ci-four-cameras.yaml";

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
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
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

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
std::vector<DecodedImuMessage> read_imu_messages(const std::string& mcap_path) {
    // NOLINTNEXTLINE(misc-const-correctness)
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

    // NOLINTNEXTLINE(misc-const-correctness)
    std::vector<DecodedImuMessage> messages;
    for (const auto& view : reader.readMessages()) {
        if (view.channel == nullptr || !view.channel->topic.starts_with("/imu/")) {
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

void open_writer(mcap::McapWriter& writer, const std::string& path) {
    mcap::McapWriterOptions options("core");
    options.compression = mcap::Compression::Zstd;
    const auto status = writer.open(path, options);
    ASSERT_TRUE(status.ok()) << status.message;
}

core::SensorStream make_accelerometer_stream(mcap::McapWriter& writer) {
    mcap::Schema imu_schema("core/ImuXYZ", "schema", core::ImuSchema.data());
    writer.addSchema(imu_schema);

    mcap::Channel imu_channel("/imu/accelerometer", "binary", imu_schema.id);
    writer.addChannel(imu_channel);

    core::SensorStream stream;
    stream.channel_id = imu_channel.id;
    stream.name = "accelerometer";
    stream.reader = std::make_unique<core::SharedDictReader>("accelerometer");
    stream.type = core::StreamType::IMU;
    stream.num_frames = 4000U;
    return stream;
}

} // namespace

TEST(LoggerTest, ProcessSensorStreamWritesImuSampleToMcap) {
    // GIVEN: Shared memory, an accelerometer writer, and an MCAP stream processor are ready
    TempMcapPath const output_path;
    const core::SharedDictMaster shared_dict_master(kTestConfigPath);
    core::WriterConfig const writer_config;
    core::SharedDictWriter shared_dict_writer("accelerometer", writer_config);
    const core::SharedDictClient shared_dict_client("accelerometer");
    ASSERT_TRUE(shared_dict_client.is_ready());

    {
        mcap::McapWriter writer;
        open_writer(writer, output_path.string());
        std::mutex writer_mutex;
        core::SharedDictStreamProcessor stream_processor(writer, writer_mutex, 90);
        auto stream = make_accelerometer_stream(writer);

        ASSERT_NE(stream.reader, nullptr);
        ASSERT_TRUE(stream.reader->is_ready());
        EXPECT_NE(stream.channel_id, 0U);

        // WHEN: An IMU sample is published and the stream processor handles the stream
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

        stream_processor.process(stream);

        std::lock_guard<std::mutex> const lock(writer_mutex);
        writer.close();
    }

    // THEN: The processor writes one IMU message with the sample payload and metadata
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
    // GIVEN: An MCAP stream processor exists without a shared-memory master
    TempMcapPath const output_path;

    {
        mcap::McapWriter writer;
        open_writer(writer, output_path.string());
        std::mutex writer_mutex;
        core::SharedDictStreamProcessor stream_processor(writer, writer_mutex, 90);
        auto stream = make_accelerometer_stream(writer);

        ASSERT_NE(stream.reader, nullptr);
        EXPECT_FALSE(stream.reader->is_ready());

        // WHEN: The unavailable stream is processed
        stream_processor.process(stream);

        std::lock_guard<std::mutex> const lock(writer_mutex);
        writer.close();
    }

    // THEN: No IMU messages are written
    const auto messages = read_imu_messages(output_path.string());
    EXPECT_TRUE(messages.empty());
}
