#include <gtest/gtest.h>

#include "../ringbuffer.hpp"
#include "../utils.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace {

TEST(SharedMemoryLayoutTest, DataFrameHeaderHasExpectedStableShape) {
    // GIVEN: The shared-memory data frame layout is used as a binary interface

    // WHEN: The field offsets are inspected

    // THEN: The metadata fields keep their expected positions before payload data
    EXPECT_EQ(offsetof(core::DataFrame, timestamp_ns), 0U);
    EXPECT_EQ(offsetof(core::DataFrame, checksum), sizeof(uint64_t));
    EXPECT_EQ(offsetof(core::DataFrame, sequence), sizeof(uint64_t) + sizeof(uint32_t));
    EXPECT_EQ(offsetof(core::DataFrame, data), sizeof(uint64_t) + 2U * sizeof(uint32_t));
}

TEST(SharedMemoryLayoutTest, BufferHeaderStartsWithRingbufferState) {
    // GIVEN: The shared-memory buffer layout is used as a binary interface

    // WHEN: The buffer header offsets are inspected

    // THEN: The ringbuffer state and name fields keep their expected positions
    EXPECT_EQ(offsetof(core::Buffer, head), 0U);
    EXPECT_EQ(offsetof(core::Buffer, sequence), sizeof(std::atomic<uint32_t>));
    EXPECT_EQ(offsetof(core::Buffer, name), 2U * sizeof(std::atomic<uint32_t>));
    EXPECT_EQ(sizeof(core::Buffer::name), 32U);
    EXPECT_LT(offsetof(core::Buffer, frames), offsetof(core::Buffer, offset) + sizeof(uint64_t) + 8U);
}

TEST(SharedMemoryLayoutTest, LayoutTracksNamedBuffersAndPayloadEntries) {
    // GIVEN: A data entry carries a named payload and metadata
    core::DataEntry const entry{"camera", {1U, 2U, 3U}, 4U, 5U, 6U};

    // WHEN: The entry and shared-memory namespace are inspected

    // THEN: The namespace and entry fields expose the expected values
    EXPECT_EQ(core::SHM_NAME, "/shared_dict");
    EXPECT_EQ(entry.key, "camera");
    EXPECT_EQ(entry.data.size(), 3U);
    EXPECT_EQ(entry.head, 4U);
    EXPECT_EQ(entry.sequence, 5U);
    EXPECT_EQ(entry.timestamp_ns, 6U);
}

} // namespace
