#ifndef WORKSPACES_CORE_CORE_UTILS_CONFIGS_HPP
#define WORKSPACES_CORE_CORE_UTILS_CONFIGS_HPP

#include <string>
#include <vector>

namespace core {

struct CameraConfig {
    std::string name;
    std::string device;
    int width;
    int height;
    int fps;

};

struct RootConfig {
    std::vector<CameraConfig> cameras;
};

void declare_config(CameraConfig& config);
void declare_config(RootConfig& config);

class Config {
public:
    void load(const std::string& config_path);
    const RootConfig& get_config();

private:
    RootConfig _config;
};

} // namespace core
#endif
