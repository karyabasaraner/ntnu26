#include "../base_camera.hpp"
#include "configs.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>
#include <utility>

#include <gtest/gtest.h>

namespace {

struct LifecycleCalls {
    std::atomic<uint32_t> starts{0};
    std::atomic<uint32_t> stops{0};
    std::atomic<uint32_t> cleanups{0};
};

class FakeCamera final : public core::Camera {
public:
    explicit FakeCamera(std::shared_ptr<LifecycleCalls> calls, bool start_succeeds = true)
        : Camera(core::CameraConfig{}), _calls(std::move(calls)), _start_succeeds(start_succeeds) {}

    FakeCamera(const FakeCamera&) = delete;
    FakeCamera& operator=(const FakeCamera&) = delete;
    FakeCamera(FakeCamera&&) = delete;
    FakeCamera& operator=(FakeCamera&&) = delete;

    ~FakeCamera() override { stop(); }

    bool is_valid() const override { return true; }

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

TEST(BaseCameraTest, StartAndStopTransitionsOnce) {
    // GIVEN: An initialized camera backend
    auto calls = std::make_shared<LifecycleCalls>();
    FakeCamera camera(calls);

    // WHEN: The camera is started and stopped
    EXPECT_TRUE(camera.start());
    EXPECT_TRUE(camera.is_running());
    camera.stop();

    // THEN: Acquisition and cleanup each run exactly once
    EXPECT_FALSE(camera.is_running());
    EXPECT_EQ(calls->starts, 1U);
    EXPECT_EQ(calls->stops, 1U);
    EXPECT_EQ(calls->cleanups, 1U);
}

TEST(BaseCameraTest, RepeatedLifecycleCallsAreRejectedOrIgnored) {
    // GIVEN: A running camera
    auto calls = std::make_shared<LifecycleCalls>();
    FakeCamera camera(calls);
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

TEST(BaseCameraTest, FailedStartCanBeRetriedAndStillCleansUp) {
    // GIVEN: A camera backend that rejects acquisition
    auto calls = std::make_shared<LifecycleCalls>();
    FakeCamera camera(calls, false);

    // WHEN: Starting fails twice and the camera is stopped
    EXPECT_FALSE(camera.start());
    EXPECT_FALSE(camera.start());
    camera.stop();

    // THEN: No acquisition stop is issued, but resources are cleaned once
    EXPECT_EQ(calls->starts, 2U);
    EXPECT_EQ(calls->stops, 0U);
    EXPECT_EQ(calls->cleanups, 1U);
}

TEST(BaseCameraTest, DestructionWithoutStartCleansUpBackendResources) {
    // GIVEN: An initialized camera that has not started
    auto calls = std::make_shared<LifecycleCalls>();

    // WHEN: The concrete camera is destroyed
    { FakeCamera const camera(calls); }

    // THEN: Its backend resources are cleaned exactly once
    EXPECT_EQ(calls->stops, 0U);
    EXPECT_EQ(calls->cleanups, 1U);
}

} // namespace
