#include "../event_camera.hpp"
#include "../event_record.hpp"
#include "../../shared_memory/client/reader.hpp"
#include "../../shared_memory/master.hpp"
#include "configs.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

namespace {

struct LifecycleCalls {
    std::atomic<uint32_t> starts{0};
    std::atomic<uint32_t> stops{0};
    std::atomic<uint32_t> cleanups{0};
};

class FakeEventCamera final : public core::EventCamera {
public:
    explicit FakeEventCamera(core::EventCameraConfig config, std::shared_ptr<LifecycleCalls> calls, bool start_succeeds = true)
        : EventCamera(std::move(config)), _calls(std::move(calls)), _start_succeeds(start_succeeds) {}

    FakeEventCamera(const FakeEventCamera&) = delete;
    FakeEventCamera& operator=(const FakeEventCamera&) = delete;
    FakeEventCamera(FakeEventCamera&&) = delete;
    FakeEventCamera& operator=(FakeEventCamera&&) = delete;

    ~FakeEventCamera() override { stop(); }

    bool is_valid() const override { return true; }

    void publish(const core::EventRecord* events, size_t count) { process_events(events, count); }

private:
    std::shared_ptr<LifecycleCalls> _calls;
    bool _start_succeeds;

    bool _start_acquisition() override {
        ++_calls->starts;
        return _start_succeeds;
    }

    bool _stop_acquisition() noexcept override {
        ++_calls->stops;
        return true;
    }

    void _capture_loop() override {
        while (is_running()) {
            std::this_thread::yield();
        }
    }

    void _post_stop() noexcept override { ++_calls->cleanups; }
};

TEST(BaseEventCameraTest, StartAndStopTransitionsOnce) {
    // GIVEN: An initialized event camera backend
    auto calls = std::make_shared<LifecycleCalls>();
    FakeEventCamera camera(core::EventCameraConfig{}, calls);

    // WHEN: The event camera is started and stopped
    EXPECT_TRUE(camera.start());
    EXPECT_TRUE(camera.is_running());
    camera.stop();

    // THEN: Acquisition and cleanup each run exactly once
    EXPECT_FALSE(camera.is_running());
    EXPECT_EQ(calls->starts, 1U);
    EXPECT_EQ(calls->stops, 1U);
    EXPECT_EQ(calls->cleanups, 1U);
}

TEST(BaseEventCameraTest, RepeatedLifecycleCallsAreRejectedOrIgnored) {
    // GIVEN: A running event camera
    auto calls = std::make_shared<LifecycleCalls>();
    FakeEventCamera camera(core::EventCameraConfig{}, calls);
    ASSERT_TRUE(camera.start());

    // WHEN: Start and stop are each requested again
    EXPECT_FALSE(camera.start());
    camera.stop();
    camera.stop();

    // THEN: The backend lifecycle runs only once
    EXPECT_EQ(calls->starts, 1U);
    EXPECT_EQ(calls->stops, 1U);
    EXPECT_EQ(calls->cleanups, 1U);
}

TEST(BaseEventCameraTest, FailedStartCanBeRetriedAndStillCleansUp) {
    // GIVEN: An event camera backend that rejects acquisition
    auto calls = std::make_shared<LifecycleCalls>();
    FakeEventCamera camera(core::EventCameraConfig{}, calls, false);

    // WHEN: Starting fails twice and the camera is stopped
    EXPECT_FALSE(camera.start());
    EXPECT_FALSE(camera.start());
    camera.stop();

    // THEN: No acquisition stop is issued, but resources are cleaned once
    EXPECT_EQ(calls->starts, 2U);
    EXPECT_EQ(calls->stops, 0U);
    EXPECT_EQ(calls->cleanups, 1U);
}

TEST(BaseEventCameraTest, DestructionWithoutStartCleansUpBackendResources) {
    // GIVEN: An initialized event camera that has not started
    auto calls = std::make_shared<LifecycleCalls>();

    // WHEN: The concrete event camera is destroyed
    { FakeEventCamera const camera(core::EventCameraConfig{}, calls); }

    // THEN: Its backend resources are cleaned exactly once
    EXPECT_EQ(calls->stops, 0U);
    EXPECT_EQ(calls->cleanups, 1U);
}

const std::string TEST_CONFIG_PATH = "ci/configs/ci-event-cameras.yaml";

TEST(BaseEventCameraTest, SplitsOversizedBatchAcrossMultipleFramesWithoutDroppingEvents) {
    // GIVEN: A shared memory segment sized for an event camera with capacity for 2
    // events per frame, and a matching event camera instance
    const core::SharedDictMaster shared_dict_master(TEST_CONFIG_PATH);

    core::Config config;
    config.load(TEST_CONFIG_PATH);
    const core::EventCameraConfig event_camera_config = config.get_config().event_cameras.front();
    ASSERT_EQ(event_camera_config.max_events_per_frame, 2U);

    auto calls = std::make_shared<LifecycleCalls>();
    FakeEventCamera camera(event_camera_config, calls);

    // WHEN: A batch of 5 events is published (larger than the 2-event capacity)
    std::vector<core::EventRecord> events(5);
    for (size_t index = 0; index < events.size(); ++index) {
        events[index].timestamp_ns = static_cast<int64_t>(1000 + index);
        events[index].x = static_cast<uint16_t>(index);
        events[index].y = static_cast<uint16_t>(index * 2);
        events[index].polarity = static_cast<uint8_t>(index % 2);
    }
    camera.publish(events.data(), events.size());
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // THEN: The batch is split into 3 frames (2, 2, 1 events), in order, and no event
    // is dropped
    core::SharedDictReader reader(event_camera_config.name);
    ASSERT_TRUE(reader.is_ready());

    core::DataEntry entry;

    reader.read_absolute(entry, 0);
    ASSERT_EQ(entry.sequence, 0U);
    core::EventBatchHeader header{};
    std::memcpy(&header, entry.data.data(), sizeof(header));
    EXPECT_EQ(header.num_events, 2U);

    reader.read_absolute(entry, 1);
    ASSERT_EQ(entry.sequence, 1U);
    std::memcpy(&header, entry.data.data(), sizeof(header));
    EXPECT_EQ(header.num_events, 2U);

    reader.read_absolute(entry, 2);
    ASSERT_EQ(entry.sequence, 2U);
    std::memcpy(&header, entry.data.data(), sizeof(header));
    EXPECT_EQ(header.num_events, 1U);

    core::EventRecord record{};
    std::memcpy(&record, entry.data.data() + sizeof(header), sizeof(record));
    EXPECT_EQ(record.x, 4U);
    EXPECT_EQ(record.timestamp_ns, 1004);
}

} // namespace
