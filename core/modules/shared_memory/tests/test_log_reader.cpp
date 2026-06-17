#include "../logger/log_reader.hpp"
#include "../logger/schema.hpp"

#include <gtest/gtest.h>
#include <mcap/reader.hpp>
#include <mcap/types.hpp>
#include <mcap/writer.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iterator>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

namespace {

class TempMcapPath {
public:
    TempMcapPath() :
        _path(std::filesystem::temp_directory_path() /
              std::filesystem::path("core-log-reader-test-" + std::to_string(_counter++) + ".mcap")) {}

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

void assign_payload(std::vector<std::byte>& payload, const std::string& json) {
    payload.reserve(json.size());
    std::transform(json.begin(), json.end(), std::back_inserter(payload), [](char character) {
        return static_cast<std::byte>(static_cast<unsigned char>(character));
    });
}

template <typename T>
void append_value(std::vector<std::byte>& payload, const T& value) {
    const auto old_size = payload.size();
    payload.resize(old_size + sizeof(T));
    std::span<std::byte> const output(payload);
    std::memcpy(output.subspan(old_size, sizeof(T)).data(), &value, sizeof(T));
}

std::vector<std::byte> make_imu_payload(uint64_t timestamp_ns, uint32_t sequence, const std::array<float, 3>& xyz) {
    std::ostringstream json;
    json << R"({"timestamp":{"sec":)" << (timestamp_ns / 1'000'000'000ULL)
         << R"(,"nsec":)" << (timestamp_ns % 1'000'000'000ULL)
         << R"(},"sequence":)" << sequence
         << R"(,"x":)" << xyz[0]
         << R"(,"y":)" << xyz[1]
         << R"(,"z":)" << xyz[2]
         << '}';
    std::vector<std::byte> payload;
    assign_payload(payload, json.str());
    return payload;
}

std::vector<std::byte> make_camera_payload(
    uint64_t timestamp_ns,
    const std::string& frame_id,
    const std::vector<std::byte>& jpeg_data
) {
    constexpr auto kJpegDataBase64 = "/9j/2Q==";
    static_cast<void>(jpeg_data);

    std::ostringstream json;
    json << R"({"timestamp":{"sec":)" << (timestamp_ns / 1'000'000'000ULL)
         << R"(,"nsec":)" << (timestamp_ns % 1'000'000'000ULL)
         << R"(},"frame_id":")" << frame_id
         << R"(","data":")" << kJpegDataBase64
         << R"(","format":"jpeg"})";
    std::vector<std::byte> payload;
    assign_payload(payload, json.str());
    return payload;
}

void write_message(mcap::McapWriter& writer, mcap::ChannelId channel_id, uint32_t sequence, uint64_t timestamp_ns, const std::vector<std::byte>& payload) {
    mcap::Message msg;
    msg.channelId = channel_id;
    msg.sequence = sequence;
    msg.publishTime = timestamp_ns;
    msg.logTime = timestamp_ns;
    msg.data = payload.data();
    msg.dataSize = payload.size();

    const auto status = writer.write(msg);
    ASSERT_TRUE(status.ok()) << status.message;
}

void write_test_log(const std::string& path) {
    mcap::McapWriter writer;
    mcap::McapWriterOptions options("core");
    options.compression = mcap::Compression::Zstd;
    const auto open_status = writer.open(path, options);
    ASSERT_TRUE(open_status.ok()) << open_status.message;

    mcap::Schema imu_schema(core::ImuSchemaName, core::JsonSchemaEncoding, core::ImuSchema.data());
    writer.addSchema(imu_schema);
    mcap::Channel imu_channel("/imu/accelerometer", core::JsonMessageEncoding, imu_schema.id);
    writer.addChannel(imu_channel);

    mcap::Schema image_schema(core::CompressedImageSchemaName, core::JsonSchemaEncoding, core::CompressedImageSchema.data());
    writer.addSchema(image_schema);
    mcap::Channel image_channel("/camera/front/image/compressed", core::JsonMessageEncoding, image_schema.id);
    writer.addChannel(image_channel);

    const auto imu_payload_0 = make_imu_payload(1'000'000'000ULL, 10U, {1.0F, 2.0F, 3.0F});
    const auto imu_payload_1 = make_imu_payload(2'000'000'000ULL, 11U, {4.0F, 5.0F, 6.0F});
    const std::vector<std::byte> jpeg_data{
        std::byte{0xFF},
        std::byte{0xD8},
        std::byte{0xFF},
        std::byte{0xD9},
    };
    const auto camera_payload = make_camera_payload(1'500'000'000ULL, "front", jpeg_data);

    // GIVEN: A deterministic MCAP file contains Foxglove-compatible IMU and compressed image topics
    write_message(writer, imu_channel.id, 10U, 1'000'000'000ULL, imu_payload_0);
    write_message(writer, image_channel.id, 3U, 1'500'000'000ULL, camera_payload);
    write_message(writer, imu_channel.id, 11U, 2'000'000'000ULL, imu_payload_1);
    writer.close();
}

struct FoxgloveChannelMetadataCheck {
    bool saw_imu_channel{false};
    bool saw_image_channel{false};
};

FoxgloveChannelMetadataCheck check_foxglove_channel_metadata(const std::string& path) {
    // NOLINTNEXTLINE(misc-const-correctness): MCAP reader methods mutate parser state while opening and reading.
    mcap::McapReader reader;
    const auto open_status = reader.open(path);
    if (!open_status.ok()) {
        throw std::runtime_error("Failed to open test MCAP: " + std::string(open_status.message));
    }

    const auto summary_status = reader.readSummary(mcap::ReadSummaryMethod::AllowFallbackScan);
    if (!summary_status.ok()) {
        throw std::runtime_error("Failed to read test MCAP summary: " + std::string(summary_status.message));
    }

    // NOLINTNEXTLINE(misc-const-correctness): clang-tidy does not see the mutation inside MCAP's message range loop.
    FoxgloveChannelMetadataCheck check;
    for (const auto& view : reader.readMessages()) {
        if (view.channel == nullptr || view.schema == nullptr) {
            throw std::runtime_error("Test MCAP message is missing channel or schema metadata");
        }
        if (view.channel->messageEncoding != core::JsonMessageEncoding || view.schema->encoding != core::JsonSchemaEncoding) {
            throw std::runtime_error("Test MCAP message does not use Foxglove-compatible JSON encodings");
        }

        if (view.channel->topic == "/imu/accelerometer") {
            check.saw_imu_channel = view.schema->name == core::ImuSchemaName;
        } else if (view.channel->topic == "/camera/front/image/compressed") {
            check.saw_image_channel = view.schema->name == core::CompressedImageSchemaName;
        }
    }

    return check;
}

} // namespace

TEST(LogReaderTest, ReadLogFileLoadsTopicsDataAndMetadata) {
    // GIVEN: A small Foxglove-compatible core MCAP log file was written
    TempMcapPath const path;
    write_test_log(path.string());
    const auto schema_check = check_foxglove_channel_metadata(path.string());
    EXPECT_TRUE(schema_check.saw_imu_channel);
    EXPECT_TRUE(schema_check.saw_image_channel);

    // WHEN: The log is read back into memory
    const auto log_file = core::read_log_file(path.string());

    // THEN: Topics are discoverable and typed getters return the expected data
    const auto topics = log_file.topics();
    ASSERT_EQ(topics.size(), 2U);
    EXPECT_EQ(topics[0], "/camera/front/image/compressed");
    EXPECT_EQ(topics[1], "/imu/accelerometer");

    const auto* imu = log_file.get_imu_data("/imu/accelerometer");
    ASSERT_NE(imu, nullptr);
    EXPECT_EQ(imu->timestamp_ns, (std::vector<uint64_t>{1'000'000'000ULL, 2'000'000'000ULL}));
    EXPECT_EQ(imu->sequence, (std::vector<uint32_t>{10U, 11U}));
    EXPECT_EQ(imu->x, (std::vector<float>{1.0F, 4.0F}));
    EXPECT_EQ(imu->y, (std::vector<float>{2.0F, 5.0F}));
    EXPECT_EQ(imu->z, (std::vector<float>{3.0F, 6.0F}));
    EXPECT_EQ(log_file.get_imu_data("/camera/front/image/compressed"), nullptr);

    const auto* camera = log_file.get_camera_data("/camera/front/image/compressed");
    ASSERT_NE(camera, nullptr);
    EXPECT_EQ(camera->timestamp_ns, (std::vector<uint64_t>{1'500'000'000ULL}));
    EXPECT_EQ(camera->frame_id, (std::vector<std::string>{"front"}));
    EXPECT_EQ(camera->format, (std::vector<std::string>{"jpeg"}));
    ASSERT_EQ(camera->jpeg_data.size(), 1U);
    EXPECT_EQ(camera->jpeg_data[0], (std::vector<std::byte>{std::byte{0xFF}, std::byte{0xD8}, std::byte{0xFF}, std::byte{0xD9}}));
    EXPECT_EQ(log_file.get_camera_data("/imu/accelerometer"), nullptr);

    const auto& metadata = log_file.metadata();
    EXPECT_EQ(metadata.path, path.string());
    EXPECT_GT(metadata.file_size_bytes, 0U);
    EXPECT_EQ(metadata.start_time_ns, 1'000'000'000ULL);
    EXPECT_EQ(metadata.end_time_ns, 2'000'000'000ULL);
    EXPECT_DOUBLE_EQ(metadata.duration_s, 1.0);
    ASSERT_EQ(metadata.topics.size(), 2U);

    const auto imu_metadata = metadata.topics.at("/imu/accelerometer");
    EXPECT_EQ(imu_metadata.type, core::LogTopicType::IMU);
    EXPECT_EQ(imu_metadata.message_count, 2U);
    EXPECT_GT(imu_metadata.payload_bytes, 0U);
    EXPECT_DOUBLE_EQ(imu_metadata.duration_s, 1.0);
    EXPECT_DOUBLE_EQ(imu_metadata.average_rate_hz, 1.0);

    const auto camera_metadata = metadata.topics.at("/camera/front/image/compressed");
    EXPECT_EQ(camera_metadata.type, core::LogTopicType::COMPRESSED_IMAGE);
    EXPECT_EQ(camera_metadata.message_count, 1U);
    EXPECT_GT(camera_metadata.payload_bytes, 0U);
    EXPECT_DOUBLE_EQ(camera_metadata.duration_s, 0.0);
    EXPECT_DOUBLE_EQ(camera_metadata.average_rate_hz, 0.0);
}
