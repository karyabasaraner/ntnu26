#ifndef WORKSPACES_CORE_CORE_MODULES_IMU_IMU_MODULE_HPP
#define WORKSPACES_CORE_CORE_MODULES_IMU_IMU_MODULE_HPP

#include <memory>
#include <string>

#include "../utils/configs.hpp"
#include "imu_device.hpp"

namespace core {

class IMUModule {
public:
    explicit IMUModule(const std::string& config_path);
    ~IMUModule();

    void start();
    void stop();

private:
    Config _config;
    std::vector<std::unique_ptr<IMUDevice>> _devices;
};

} // namespace core

#endif // WORKSPACES_CORE_CORE_MODULES_IMU_IMU_MODULE_HPP
