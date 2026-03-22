#include "imu_module.hpp"

#include <string>

namespace core {

IMUModule::IMUModule(const std::string& config_path) {
    _config.load(config_path);
}

void IMUModule::start() {
    // TODO(MJ): Implement IMU reading and processing logic
}

} // namespace core
