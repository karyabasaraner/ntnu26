#include "imu_module.hpp"

#include <spdlog/spdlog.h>
#include <string>

namespace core {

IMUModule::IMUModule(const std::string& config_path) {
    _config.load(config_path);
}

IMUModule::~IMUModule() {
    stop();
}

void IMUModule::start() {
    for (const auto& imu_config : _config.get_config().imus) {
        spdlog::info("Configuring {} {}", imu_config.name, imu_config.device);
        auto device = std::make_unique<IMUDevice>(imu_config);
        device->start();
        _devices.push_back(std::move(device));
    }
}

void IMUModule::stop() {
    for (auto& device : _devices) {
        device->stop();
    }
    _devices.clear();
}

} // namespace core
