#include <gtest/gtest.h>

#include "../logger/schema.hpp"

#include <string_view>

namespace {

constexpr std::string_view kCompressedImageSchema(core::CompressedImageSchema.data());
constexpr std::string_view kImuSchema(core::ImuSchema.data());

} // namespace

TEST(LoggerSchemaTest, CompressedImageSchemaMatchesExpectedFieldsAndOrder) {
    EXPECT_EQ(
        kCompressedImageSchema,
        "uint64 source_timestamp_ns\n"
        "uint32 source_sequence\n"
        "uint32 width\n"
        "uint32 height\n"
        "uint8 channels\n"
        "uint8 jpeg_quality\n"
        "bytes jpeg_data\n");
}

TEST(LoggerSchemaTest, ImuSchemaMatchesExpectedFieldsAndOrder) {
    EXPECT_EQ(
        kImuSchema,
        "uint64 source_timestamp_ns\n"
        "uint32 source_sequence\n"
        "float32 x\n"
        "float32 y\n"
        "float32 z\n");
}
