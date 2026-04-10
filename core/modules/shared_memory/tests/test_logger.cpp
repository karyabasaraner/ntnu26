#include <gtest/gtest.h>

#include "../../../tests/test_async_helpers.hpp"
#include "../client/client.hpp"
#include "../client/reader.hpp"
#include "../client/writer.hpp"
#include "../logger/log_reader.hpp"
#include "../logger/logger.hpp"
#include "../logger/schema.hpp"
#include "../master.hpp"
#include "../ringbuffer.hpp"
#include "configs.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <mcap/reader.hpp>
#include <mcap/types.hpp>
#include <mcap/writer.hpp>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace {

const std::string kTestConfigPath = "ci/configs/ci-four-cameras.yaml";
constexpr int64_t kTestSteadyToUnixOffsetNs = 1'700'000'000'000'000'000LL;

using TimestampMapper = core::SharedDictStreamProcessor::SteadyClockUnixTimeMapper;

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

struct DecodedImuMessage {
    std::string topic;
    uint64_t timestamp_ns{0};
    uint32_t sequence{0};
    float x{0.0F};
    float y{0.0F};
    float z{0.0F};
};

std::vector<DecodedImuMessage> read_imu_messages(const std::string& mcap_path) {
    const auto log_file = core::read_log_file(mcap_path);
    std::vector<DecodedImuMessage> messages;
    for (const auto& topic : log_file.imu_topics()) {
        const auto* series = log_file.get_imu_data(topic);
        if (series == nullptr) {
            continue;
        }
        for (std::size_t index = 0; index < series->timestamp_ns.size(); ++index) {
            messages.push_back(DecodedImuMessage{
                .topic = topic,
                .timestamp_ns = series->timestamp_ns[index],
                .sequence = series->sequence[index],
                .x = series->x[index],
                .y = series->y[index],
                .z = series->z[index],
            });
        }
    }

    return messages;
}

std::vector<std::pair<uint64_t, uint64_t>> read_mcap_message_times(const std::string& mcap_path) {
    // NOLINTNEXTLINE(misc-const-correctness): MCAP reader methods mutate parser state while opening and reading.
    mcap::McapReader reader;
    const auto open_status = reader.open(mcap_path);
    EXPECT_TRUE(open_status.ok()) << open_status.message;
    const auto summary_status = reader.readSummary(mcap::ReadSummaryMethod::AllowFallbackScan);
    EXPECT_TRUE(summary_status.ok()) << summary_status.message;

    // NOLINTNEXTLINE(misc-const-correctness): populated while iterating MCAP messages below.
    std::vector<std::pair<uint64_t, uint64_t>> timestamps;
    for (const auto& view : reader.readMessages()) {
        if (view.schema == nullptr || view.schema->name != core::ImuSchemaName) {
            continue;
        }
        timestamps.emplace_back(view.message.publishTime, view.message.logTime);
    }
    return timestamps;
}

void open_writer(mcap::McapWriter& writer, const std::string& path) {
    mcap::McapWriterOptions options("core");
    options.compression = mcap::Compression::Zstd;
    const auto status = writer.open(path, options);
    ASSERT_TRUE(status.ok()) << status.message;
}

core::SensorStream make_accelerometer_stream(mcap::McapWriter& writer) {
    mcap::Schema imu_schema(core::ImuSchemaName, core::JsonSchemaEncoding, core::ImuSchema.data());
    writer.addSchema(imu_schema);

    mcap::Channel imu_channel("/imu/accelerometer", core::JsonMessageEncoding, imu_schema.id);
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
        core::SharedDictStreamProcessor stream_processor(writer, writer_mutex, 90, TimestampMapper(kTestSteadyToUnixOffsetNs));
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
    EXPECT_EQ(messages[0].timestamp_ns, 1'700'000'000'000'424'242ULL);
    EXPECT_EQ(messages[0].sequence, 41U);
    EXPECT_FLOAT_EQ(messages[0].x, 1.25F);
    EXPECT_FLOAT_EQ(messages[0].y, -2.5F);
    EXPECT_FLOAT_EQ(messages[0].z, 3.75F);

    const auto mcap_times = read_mcap_message_times(output_path.string());
    ASSERT_EQ(mcap_times.size(), 1U);
    EXPECT_EQ(mcap_times[0].first, 1'700'000'000'000'424'242ULL);
    EXPECT_EQ(mcap_times[0].second, 1'700'000'000'000'424'242ULL);
}

TEST(LoggerTest, ProcessSensorStreamWithUnavailableSharedMemoryWritesNoMessages) {
    // GIVEN: An MCAP stream processor exists without a shared-memory master
    TempMcapPath const output_path;

    {
        mcap::McapWriter writer;
        open_writer(writer, output_path.string());
        std::mutex writer_mutex;
        core::SharedDictStreamProcessor stream_processor(writer, writer_mutex, 90, TimestampMapper(kTestSteadyToUnixOffsetNs));
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

TEST(LoggerTest, SteadyClockUnixTimeMapperPreservesTimestampDurations) {
    // GIVEN: A deterministic steady-to-Unix timestamp mapper
    const TimestampMapper mapper(1'700'000'000'000'000'000LL);

    // WHEN: Two steady-clock timestamps are mapped into Unix time
    const auto first_timestamp = mapper.to_unix_time_ns(1'000U);
    const auto second_timestamp = mapper.to_unix_time_ns(2'500U);

    // THEN: The absolute timestamps are shifted while their interval remains unchanged
    EXPECT_EQ(first_timestamp, 1'700'000'000'000'001'000ULL);
    EXPECT_EQ(second_timestamp, 1'700'000'000'000'002'500ULL);
    EXPECT_EQ(second_timestamp - first_timestamp, 1'500U);
}
