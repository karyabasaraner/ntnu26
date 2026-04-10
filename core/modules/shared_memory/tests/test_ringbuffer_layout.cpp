#include <gtest/gtest.h>

#include "../ringbuffer.hpp"
#include "../utils.hpp"

#include <cstddef>
#include <cstdint>
#include <string>

namespace {

TEST(SharedMemoryLayoutTest, DataFrameHeaderHasExpectedStableShape) {
    EXPECT_EQ(offsetof(core::DataFrame, timestamp_ns), 0U);
    EXPECT_EQ(offsetof(core::DataFrame, checksum), sizeof(uint64_t));
    EXPECT_EQ(offsetof(core::DataFrame, sequence), sizeof(uint64_t) + sizeof(uint32_t));
    EXPECT_EQ(offsetof(core::DataFrame, data), sizeof(uint64_t) + 2U * sizeof(uint32_t));
}

TEST(SharedMemoryLayoutTest, BufferHeaderStartsWithRingbufferState) {
    EXPECT_EQ(offsetof(core::Buffer, head), 0U);
    EXPECT_EQ(offsetof(core::Buffer, sequence), sizeof(std::atomic<uint32_t>));
    EXPECT_EQ(offsetof(core::Buffer, name), 2U * sizeof(std::atomic<uint32_t>));
    EXPECT_EQ(sizeof(core::Buffer::name), 32U);
    EXPECT_LT(offsetof(core::Buffer, frames), offsetof(core::Buffer, offset) + sizeof(uint64_t) + 8U);
}

TEST(SharedMemoryLayoutTest, LayoutTracksNamedBuffersAndPayloadEntries) {
    core::DataEntry entry{"camera", {1U, 2U, 3U}, 4U, 5U, 6U};

    EXPECT_EQ(core::SHM_NAME, "/shared_dict");
    EXPECT_EQ(entry.key, "camera");
    EXPECT_EQ(entry.data.size(), 3U);
    EXPECT_EQ(entry.head, 4U);
    EXPECT_EQ(entry.sequence, 5U);
    EXPECT_EQ(entry.timestamp_ns, 6U);
}

} // namespace
