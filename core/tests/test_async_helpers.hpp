#ifndef WORKSPACES_CORE_CORE_TESTS_TEST_ASYNC_HELPERS_HPP
#define WORKSPACES_CORE_CORE_TESTS_TEST_ASYNC_HELPERS_HPP

#include <chrono>
#include <functional>
#include <thread>

namespace core::test {

inline bool wait_for_predicate(
    const std::function<bool()>& predicate,
    std::chrono::milliseconds timeout = std::chrono::milliseconds(250),
    std::chrono::milliseconds poll_interval = std::chrono::milliseconds(5)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(poll_interval);
    }
    return predicate();
}

} // namespace core::test

#endif
