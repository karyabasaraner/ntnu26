#ifndef WORKSPACES_CORE_CORE_UTILS_CONFIGS_HPP
#define WORKSPACES_CORE_CORE_UTILS_CONFIGS_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace core {

struct RingBufferConfig {
    size_t num_frames;
    size_t size_per_frame;
    std::string name;
};

struct TransformConfig {
    std::string name;
};

struct SettingsConfig {
    int value;
    std::string name;
    uint32_t id{0};
};

struct WriterConfig {
    size_t height{0};
    size_t width{0};
    std::vector<TransformConfig> transforms;
};

struct CameraConfig {
    size_t fps;
    size_t req_buffer_count;
    std::string device;
    std::string format;
    std::string name;
    std::vector<SettingsConfig> settings;
    std::vector<WriterConfig> writer;
    uint32_t subsample_factor{1};
};

struct IMUConfig {
    float sampling_frequency;
    float scale;
    size_t buffer_samples;
    size_t watermark_samples;
    std::string device;
    std::string name;
    std::vector<std::string> channels;
    std::vector<WriterConfig> writer;
};

struct RootConfig {
    std::vector<CameraConfig> cameras;
    std::vector<IMUConfig> imus;
    std::vector<RingBufferConfig> shared_memory;
};

void declare_config(CameraConfig& config);
void declare_config(IMUConfig& config);
void declare_config(RootConfig& config);
void declare_config(WriterConfig& config);

class Config {
public:
    void load(const std::string& config_path);
    const RootConfig& get_config();

private:
    RootConfig _config;
};

} // namespace core
#endif
