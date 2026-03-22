#ifndef WORKSPACES_CORE_CORE_MODULES_IMU_IMU_MODULE_HPP
#define WORKSPACES_CORE_CORE_MODULES_IMU_IMU_MODULE_HPP

#include "../utils/configs.hpp"

namespace core {

class IMUModule {
public:
    explicit IMUModule(const std::string& config_path);
    void start();

private:
    Config _config;
};

} // namespace core

#endif // WORKSPACES_CORE_CORE_MODULES_IMU_IMU_MODULE_HPP
