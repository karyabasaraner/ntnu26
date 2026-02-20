#ifndef WORKSPACES_CORE_CORE_UTILS_CONFIGS_HPP
#define WORKSPACES_CORE_CORE_UTILS_CONFIGS_HPP

#include <string>
#include <vector>

namespace core {

// Transforms applied sequentially to the raw data
struct TransformConfig {
    std::string name;
};

struct CameraConfig {
    int fps;
    int height;
    int req_buffer_count;
    int width;
    std::string device;
    std::string format;
    std::string name;
    std::vector<TransformConfig> transforms;
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
