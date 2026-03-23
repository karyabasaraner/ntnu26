#include "imu_module.hpp"

#include <spdlog/spdlog.h>
#include <string>

namespace core {

IMUModule::IMUModule(const std::string& config_path) {
    _config.load(config_path);
    _initialize_imus();
}

IMUModule::~IMUModule() {
    stop_imus();
}

void IMUModule::_initialize_imus() {
    for (const auto& imu_config : _config.get_config().imus) {
        spdlog::info("Configuring {} {}", imu_config.name, imu_config.device);
        auto device = std::make_unique<IMUDevice>(imu_config);
        _devices.push_back(std::move(device));
    }
}

uint8_t IMUModule::get_num_imus() const {
    return static_cast<uint8_t>(_devices.size());
}

void IMUModule::start_imus() {
    for (auto& device : _devices) {
        device->start();
    }
    spdlog::info("Started {} IMU devices", _devices.size());
}

void IMUModule::stop_imus() {
    for (auto& device : _devices) {
        device->stop();
    }
    _devices.clear();
}

} // namespace core
