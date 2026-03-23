#ifndef WORKSPACES_CORE_CORE_UTILS_CONFIGS_HPP
#define WORKSPACES_CORE_CORE_UTILS_CONFIGS_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace core {

struct RingBufferConfig {
    std::string name;
    size_t size_per_frame;
    size_t num_frames;
};

struct TransformConfig {
    std::string name;
};

struct SettingsConfig {
    std::string name;
    int value;
    uint32_t id{0};
};

struct CameraConfig {
    size_t fps;
    size_t height;
    size_t req_buffer_count;
    size_t width;
    std::string device;
    std::string format;
    std::string name;
    std::vector<SettingsConfig> settings;
    std::vector<TransformConfig> transforms;
    uint32_t subsample_factor{1};
};

struct IMUConfig {
    std::string name;
    std::string device;
    double sampling_frequency;
    double scale;
    std::vector<std::string> channels;
};

struct RootConfig {
    std::vector<CameraConfig> cameras;
    std::vector<IMUConfig> imus;
    std::vector<RingBufferConfig> shared_memory;
};

void declare_config(CameraConfig& config);
void declare_config(IMUConfig& config);
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
