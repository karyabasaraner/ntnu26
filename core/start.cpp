#include <atomic>
#include <chrono>
#include <csignal>
#include <exception>
#include <iostream>
#include <spdlog/spdlog.h>
#include <string>
#include <thread>

#include "modules/camera/camera_module.hpp"
#include "modules/event_camera/event_camera_module.hpp"
#include "modules/imu/imu_module.hpp"
#include "modules/shared_memory/master.hpp"

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
static std::atomic<bool> g_running{true};

// NOLINTNEXTLINE(misc-unused-parameters)
static void handle_signal(int signal) {
    g_running.store(false, std::memory_order_relaxed);
}

static void print_usage(const char* argv0) {
    std::cerr << "Usage: " << argv0 << " <config_path>\n";
}

int main(int argc, char** argv) {
    if (argc < 2) {
        //NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
        print_usage(argv[0]);
        return 1;
    }
    //NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    const std::string config_path = argv[1];

    // NOLINTNEXTLINE(cert-err33-c)
    std::signal(SIGINT, handle_signal);
    // NOLINTNEXTLINE(cert-err33-c)
    std::signal(SIGTERM, handle_signal);

    try {
        spdlog::info("Starting SharedDictMaster with config: {}", config_path);
        core::SharedDictMaster const master(config_path);

        // Starting cameras
        spdlog::info("Starting CameraModule with config: {}", config_path);
        core::CameraModule cameras(config_path);
        cameras.start_cameras(); // start all cameras defined in config

        // Starting event cameras
        spdlog::info("Starting EventCameraModule with config: {}", config_path);
        core::EventCameraModule event_cameras(config_path);
        event_cameras.start_event_cameras(); // start all event cameras defined in config

        // Starting IMU
        spdlog::info("Starting IMUModule with config: {}", config_path);
        core::IMUModule imus(config_path);
        imus.start_imus();

        spdlog::info("Service running. Press Ctrl+C to stop.");

        while (g_running.load(std::memory_order_relaxed)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }

        spdlog::info("Stopping IMU...");
        imus.stop_imus();

        spdlog::info("Stopping event cameras...");
        event_cameras.stop_event_cameras();

        spdlog::info("Stopping cameras...");
        cameras.stop_cameras();

        spdlog::info("Shutting down SharedDictMaster...");
        // master goes out of scope here; destructor will clean up and shm_unlink
        return 0;
    } catch (const std::exception& ex) {
        spdlog::error("Fatal error: {}", ex.what());
        return 2;
    }
}
