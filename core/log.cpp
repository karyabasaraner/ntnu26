#include <atomic>
#include <csignal>
#include <exception>
#include <iostream>
#include <string>

#include "modules/shared_memory/logger/logger.hpp"

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
static std::atomic<bool> g_running{true};
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
static core::SharedDictLogger* g_logger{nullptr};

// NOLINTNEXTLINE(misc-unused-parameters)
static void handle_signal(int signal) {
    g_running.store(false, std::memory_order_relaxed);
    if (g_logger != nullptr) {
        g_logger->request_stop();
    }
}

static void print_usage(const char* argv0) {
    std::cerr << "Usage: " << argv0 << " <config_path> <output_mcap>\n";
}

int main(int argc, char** argv) {
    if (argc < 3) {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
        print_usage(argv[0]);
        return 1;
    }

    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    const std::string config_path = argv[1];
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    const std::string output_path = argv[2];

    // NOLINTNEXTLINE(cert-err33-c)
    std::signal(SIGINT, handle_signal);
    // NOLINTNEXTLINE(cert-err33-c)
    std::signal(SIGTERM, handle_signal);

    try {
        core::SharedDictLogger logger(config_path, output_path);
        g_logger = &logger;
        logger.run();
        g_logger = nullptr;
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "Logger failed: " << ex.what() << '\n';
        return 2;
    }
}
