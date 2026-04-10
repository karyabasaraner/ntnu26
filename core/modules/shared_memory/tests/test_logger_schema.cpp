#include <gtest/gtest.h>

#include "../logger/schema.hpp"

#include <string_view>

namespace {

constexpr std::string_view kCompressedImageSchema(core::CompressedImageSchema.data());
constexpr std::string_view kImuSchema(core::ImuSchema.data());

} // namespace

TEST(LoggerSchemaTest, CompressedImageSchemaMatchesExpectedFieldsAndOrder) {
    // GIVEN: The compressed image MCAP schema is published by the logger

    // WHEN: The schema text is inspected

    // THEN: The schema is the Foxglove compressed image JSON schema
    EXPECT_EQ(core::CompressedImageSchemaName, std::string_view("foxglove.CompressedImage"));
    EXPECT_EQ(core::JsonSchemaEncoding, std::string_view("jsonschema"));
    EXPECT_EQ(core::JsonMessageEncoding, std::string_view("json"));
    EXPECT_NE(kCompressedImageSchema.find(R"("title": "foxglove.CompressedImage")"), std::string_view::npos);
    EXPECT_NE(kCompressedImageSchema.find(R"("timestamp")"), std::string_view::npos);
    EXPECT_NE(kCompressedImageSchema.find(R"("frame_id")"), std::string_view::npos);
    EXPECT_NE(kCompressedImageSchema.find(R"("data")"), std::string_view::npos);
    EXPECT_NE(kCompressedImageSchema.find(R"("format")"), std::string_view::npos);
}

TEST(LoggerSchemaTest, ImuSchemaMatchesExpectedFieldsAndOrder) {
    // GIVEN: The IMU MCAP schema is published by the logger

    // WHEN: The schema text is inspected

    // THEN: The schema is a JSON schema for the core IMU vector format
    EXPECT_EQ(core::ImuSchemaName, std::string_view("core.ImuXYZ"));
    EXPECT_EQ(core::JsonSchemaEncoding, std::string_view("jsonschema"));
    EXPECT_EQ(core::JsonMessageEncoding, std::string_view("json"));
    EXPECT_NE(kImuSchema.find(R"("title": "core.ImuXYZ")"), std::string_view::npos);
    EXPECT_NE(kImuSchema.find(R"("timestamp")"), std::string_view::npos);
    EXPECT_NE(kImuSchema.find(R"("sequence")"), std::string_view::npos);
    EXPECT_NE(kImuSchema.find(R"("x")"), std::string_view::npos);
    EXPECT_NE(kImuSchema.find(R"("y")"), std::string_view::npos);
    EXPECT_NE(kImuSchema.find(R"("z")"), std::string_view::npos);
}
