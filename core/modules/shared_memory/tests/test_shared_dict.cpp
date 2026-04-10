#include <gtest/gtest.h>
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <functional>
#include <stdexcept>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>
#include <zlib.h>

#include "../../../utils/configs.hpp"
#include "../client/client.hpp"
#include "../client/reader.hpp"
#include "../client/writer.hpp"
#include "../master.hpp"
#include "../ringbuffer.hpp"
#include "../utils.hpp"

const std::string TEST_CONFIG_PATH = "ci/configs/ci-four-cameras.yaml";

namespace {

class ScopedShmView {
public:
    ScopedShmView() : _fd(shm_open(core::SHM_NAME.c_str(), O_RDWR, 0666)) {
        if (_fd < 0) {
            throw std::runtime_error("Failed to open shared memory for inspection");
        }

        struct stat stats {};
        if (fstat(_fd, &stats) != 0) {
            ::close(_fd);
            _fd = -1;
            throw std::runtime_error("Failed to stat shared memory for inspection");
        }

        _size = static_cast<std::size_t>(stats.st_size);
        _map = mmap(nullptr, _size, PROT_READ | PROT_WRITE, MAP_SHARED, _fd, 0);
        if (_map == MAP_FAILED) {
            _map = nullptr;
            ::close(_fd);
            _fd = -1;
            throw std::runtime_error("Failed to map shared memory for inspection");
        }
    }

    ~ScopedShmView() {
        if (_map != nullptr) {
            munmap(_map, _size);
        }
        if (_fd >= 0) {
            ::close(_fd);
        }
    }

    ScopedShmView(const ScopedShmView&) = delete;
    ScopedShmView& operator=(const ScopedShmView&) = delete;
    ScopedShmView(ScopedShmView&&) = delete;
    ScopedShmView& operator=(ScopedShmView&&) = delete;

    core::Layout* layout() const {
        return static_cast<core::Layout*>(_map);
    }

    void* map() const {
        return _map;
    }

    std::size_t size() const {
        return _size;
    }

private:
    int _fd{-1};
    void* _map{nullptr};
    std::size_t _size{0};
};

bool wait_for_predicate(const std::function<bool()>& predicate,
                        std::chrono::milliseconds timeout = std::chrono::milliseconds(250)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return predicate();
}

core::WriterConfig test_writer_config() {
    core::WriterConfig config;
    config.transforms = {};
    return config;
}

} // namespace

TEST(SharedDictTest, InitializeSharedDictMaster) {
    // WHEN: SharedDictMaster is initialized with config with 4 cameras
    const core::SharedDictMaster shared_dict_master(TEST_CONFIG_PATH);

    // THEN: SharedDictMaster should be initialized successfully and ready to use
    EXPECT_TRUE(shared_dict_master.is_initialized());
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST(SharedDictTest, MasterInitializesExpectedBufferMetadata) {
    const core::SharedDictMaster shared_dict_master(TEST_CONFIG_PATH);
    core::Config config;
    config.load(TEST_CONFIG_PATH);

    const ScopedShmView shm_view;
    core::Layout* const layout = shm_view.layout();
    ASSERT_NE(layout, nullptr);

    const auto& shared_memory_config = config.get_config().shared_memory;
    ASSERT_EQ(layout->num_buffers, shared_memory_config.size());

    auto* base = static_cast<std::byte*>(shm_view.map());
    std::size_t current_offset = offsetof(core::Layout, buffers);

    for (const auto& expected_buffer : shared_memory_config) {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic,cppcoreguidelines-pro-type-reinterpret-cast)
        auto* buffer = reinterpret_cast<core::Buffer*>(base + current_offset);
        ASSERT_NE(buffer, nullptr);

        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-array-to-pointer-decay,hicpp-no-array-decay)
        EXPECT_STREQ(buffer->name, expected_buffer.name.c_str());
        EXPECT_EQ(buffer->head.load(std::memory_order_acquire), 0U);
        EXPECT_EQ(buffer->sequence.load(std::memory_order_acquire), 0U);
        EXPECT_EQ(buffer->offset, current_offset);
        EXPECT_EQ(buffer->num_frames, expected_buffer.num_frames);
        EXPECT_EQ(buffer->size_per_frame, expected_buffer.size_per_frame);

        current_offset += offsetof(core::Buffer, frames) +
                          expected_buffer.num_frames *
                              (offsetof(core::DataFrame, data) + expected_buffer.size_per_frame);
    }

    EXPECT_EQ(current_offset, shm_view.size());
}

TEST(SharedDictTest, MasterDestructorUnlinksSharedMemoryNamespace) {
    {
        const core::SharedDictMaster shared_dict_master(TEST_CONFIG_PATH);
        const ScopedShmView shm_view;
        EXPECT_NE(shm_view.layout(), nullptr);
    }

    errno = 0;
    const int shm_fd = shm_open(core::SHM_NAME.c_str(), O_RDWR, 0666);
    EXPECT_EQ(shm_fd, -1);
    EXPECT_EQ(errno, ENOENT);
}

TEST(ShareDictTest, IntializeShareDictNoMaster) {
    // GIVEN: SharedDictClient without master is not ready
    const std::string name = "right";

    // WHEN: SharedDictClient is initialized with config for a camera
    core::SharedDictClient const shared_dict_client(name);

    // THEN: SharedDictClient should not be ready to write
    EXPECT_FALSE(shared_dict_client.is_ready());
}

TEST(ShareDictTest, InitializeSharedDictClientWithMaster) {
    // GIVEN: A shm master that is setup
    const core::SharedDictMaster shared_dict_master(TEST_CONFIG_PATH);

    // WHEN: SharedDictClient is initialized with config for a camera
    const std::string name = "right";
    core::SharedDictClient const shared_dict_client(name);

    // THEN: SharedDictClient should be ready to write
    EXPECT_TRUE(shared_dict_client.is_ready());
}

TEST(ShareDictTest, SharedDictClientIsNotReadyWhenNamedBufferIsMissing) {
    const core::SharedDictMaster shared_dict_master(TEST_CONFIG_PATH);

    core::SharedDictClient const shared_dict_client("missing_buffer");

    EXPECT_FALSE(shared_dict_client.is_ready());
    EXPECT_EQ(shared_dict_client.get_buffer(), nullptr);
}

TEST(ShareDictTest, GetHeadWrapsRelativeToRingbufferHead) {
    const core::SharedDictMaster shared_dict_master(TEST_CONFIG_PATH);
    const core::SharedDictClient shared_dict_client("right");

    ASSERT_TRUE(shared_dict_client.is_ready());
    core::Buffer* const buffer = shared_dict_client.get_buffer();
    ASSERT_NE(buffer, nullptr);

    buffer->head.store(17, std::memory_order_release);
    const uint32_t current_head = buffer->head.load(std::memory_order_acquire);
    const auto num_frames = static_cast<int32_t>(buffer->num_frames);

    EXPECT_EQ(shared_dict_client.get_head(0), current_head);
    EXPECT_EQ(shared_dict_client.get_head(1), static_cast<uint32_t>((static_cast<int32_t>(current_head) - 1 + num_frames) % num_frames));
    EXPECT_EQ(shared_dict_client.get_head(-1), static_cast<uint32_t>((static_cast<int32_t>(current_head) + 1) % num_frames));
}

TEST(ShareDictTest, GetFrameByIndexReturnsExpectedFrameStride) {
    const core::SharedDictMaster shared_dict_master(TEST_CONFIG_PATH);
    core::SharedDictClient shared_dict_client("right");

    ASSERT_TRUE(shared_dict_client.is_ready());
    core::Buffer* const buffer = shared_dict_client.get_buffer();
    ASSERT_NE(buffer, nullptr);
    ASSERT_GE(buffer->num_frames, 2U);

    core::DataFrame* const first_frame = shared_dict_client.get_frame_by_index(0);
    core::DataFrame* const second_frame = shared_dict_client.get_frame_by_index(1);

    ASSERT_NE(first_frame, nullptr);
    ASSERT_NE(second_frame, nullptr);

    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    const auto first_frame_address = reinterpret_cast<std::uintptr_t>(first_frame);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    const auto second_frame_address = reinterpret_cast<std::uintptr_t>(second_frame);
    const auto actual_stride = static_cast<std::ptrdiff_t>(second_frame_address - first_frame_address);
    const auto expected_stride =
        static_cast<std::ptrdiff_t>(offsetof(core::DataFrame, data) + buffer->size_per_frame);

    EXPECT_EQ(actual_stride, expected_stride);
}

TEST(ShareDictTest, ReaderReadAbsoluteReturnsFramePayloadAndMetadata) {
    const core::SharedDictMaster shared_dict_master(TEST_CONFIG_PATH);
    core::SharedDictClient shared_dict_client("accelerometer");
    core::SharedDictReader shared_dict_reader("accelerometer");

    ASSERT_TRUE(shared_dict_client.is_ready());
    ASSERT_TRUE(shared_dict_reader.is_ready());

    core::Buffer* const buffer = shared_dict_client.get_buffer();
    ASSERT_NE(buffer, nullptr);

    core::DataFrame* const frame = shared_dict_client.get_frame_by_index(0);
    ASSERT_NE(frame, nullptr);

    const std::vector<uint8_t> payload{1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U, 9U, 10U, 11U, 12U};
    ASSERT_EQ(buffer->size_per_frame, payload.size());
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-array-to-pointer-decay,hicpp-no-array-decay)
    std::copy(payload.begin(), payload.end(), frame->data);
    frame->checksum = crc32(0, payload.data(), static_cast<unsigned int>(payload.size()));
    frame->sequence = 77U;
    frame->timestamp_ns = 123456789U;

    buffer->head.store(1U, std::memory_order_release);

    core::DataEntry entry{};
    shared_dict_reader.read_absolute(entry, 0);

    EXPECT_EQ(entry.key, "accelerometer");
    EXPECT_EQ(entry.data, payload);
    EXPECT_EQ(entry.head, 0U);
    EXPECT_EQ(entry.sequence, 77U);
    EXPECT_EQ(entry.timestamp_ns, 123456789U);
}

TEST(ShareDictTest, ReaderSkipsFramesWithChecksumMismatch) {
    const core::SharedDictMaster shared_dict_master(TEST_CONFIG_PATH);
    core::SharedDictClient shared_dict_client("accelerometer");
    core::SharedDictReader shared_dict_reader("accelerometer");

    ASSERT_TRUE(shared_dict_client.is_ready());
    ASSERT_TRUE(shared_dict_reader.is_ready());

    core::DataFrame* const frame = shared_dict_client.get_frame_by_index(0);
    ASSERT_NE(frame, nullptr);

    const std::vector<uint8_t> payload{12U, 11U, 10U, 9U, 8U, 7U, 6U, 5U, 4U, 3U, 2U, 1U};
    core::Buffer* const buffer = shared_dict_client.get_buffer();
    ASSERT_NE(buffer, nullptr);
    ASSERT_EQ(buffer->size_per_frame, payload.size());
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-array-to-pointer-decay,hicpp-no-array-decay)
    std::copy(payload.begin(), payload.end(), frame->data);
    frame->checksum = 0U;
    frame->sequence = 91U;
    frame->timestamp_ns = 42U;

    core::DataEntry entry{};
    shared_dict_reader.read_absolute(entry, 0);

    EXPECT_TRUE(entry.key.empty());
    EXPECT_TRUE(entry.data.empty());
    EXPECT_EQ(entry.sequence, 0U);
    EXPECT_EQ(entry.timestamp_ns, 0U);
}

TEST(ShareDictTest, WriterPublishesPayloadToSharedMemory) {
    const core::SharedDictMaster shared_dict_master(TEST_CONFIG_PATH);
    const core::SharedDictClient shared_dict_client("accelerometer");
    core::SharedDictReader shared_dict_reader("accelerometer");
    core::SharedDictWriter shared_dict_writer("accelerometer", test_writer_config());

    ASSERT_TRUE(shared_dict_client.is_ready());
    ASSERT_TRUE(shared_dict_reader.is_ready());

    core::Buffer* const buffer = shared_dict_client.get_buffer();
    ASSERT_NE(buffer, nullptr);

    const std::vector<uint8_t> payload{1U, 3U, 5U, 7U, 9U, 11U, 13U, 15U, 17U, 19U, 21U, 23U};
    ASSERT_EQ(buffer->size_per_frame, payload.size());

    shared_dict_writer.add("accelerometer", payload.data(), payload.size(), 41U, 424242U);

    ASSERT_TRUE(wait_for_predicate([buffer] {
        return buffer->sequence.load(std::memory_order_acquire) == 41U;
    }));
    EXPECT_EQ(buffer->head.load(std::memory_order_acquire), 1U);

    core::DataEntry entry{};
    shared_dict_reader.read_latest(entry);

    EXPECT_EQ(entry.key, "accelerometer");
    EXPECT_EQ(entry.data, payload);
    EXPECT_EQ(entry.head, 0U);
    EXPECT_EQ(entry.sequence, 41U);
    EXPECT_EQ(entry.timestamp_ns, 424242U);
}

TEST(ShareDictTest, WriterRejectsOversizedPayloadWithoutAdvancingRingbuffer) {
    const core::SharedDictMaster shared_dict_master(TEST_CONFIG_PATH);
    const core::SharedDictClient shared_dict_client("accelerometer");
    core::SharedDictWriter shared_dict_writer("accelerometer", test_writer_config());

    ASSERT_TRUE(shared_dict_client.is_ready());

    core::Buffer* const buffer = shared_dict_client.get_buffer();
    ASSERT_NE(buffer, nullptr);

    const uint32_t initial_head = buffer->head.load(std::memory_order_acquire);
    const uint32_t initial_sequence = buffer->sequence.load(std::memory_order_acquire);

    std::vector<uint8_t> payload(buffer->size_per_frame + 1U, 0xABU);
    shared_dict_writer.add("accelerometer", payload.data(), payload.size(), 99U, 123U);

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    EXPECT_EQ(buffer->head.load(std::memory_order_acquire), initial_head);
    EXPECT_EQ(buffer->sequence.load(std::memory_order_acquire), initial_sequence);
}
