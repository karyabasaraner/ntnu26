#ifndef CONFIG_HPP
#define CONFIG_HPP

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
    void load(std::string& config_path);

private:
    RootConfig _config;
};

} // namespace core
#endif