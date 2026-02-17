#include <config_utilities/config.h>
#include <config_utilities/parsing/yaml.h>
#include <iostream>

#include "configs.hpp"

namespace core {

void declare_config(CameraConfig& config) {
    config::name("CameraConfig");
    config::field(config.name, "name", "Name of the camera");
    config::field(config.device, "device", "Device path of the camera");
    config::field(config.width, "width", "Image width in pixels");
    config::field(config.height, "height", "Image height in pixels");
    config::field(config.fps, "fps", "Frames per second");
}

void declare_config(RootConfig& config) {
    config::name("RootConfig");
    config::field(config.cameras, "cameras", "List of camera configurations");
}

void Config::load(std::string& config_path) {
    _config = config::fromYamlFile<RootConfig>(config_path);
    std::cout << "Config loaded from: " << config_path << std::endl;
}

} // namespace core