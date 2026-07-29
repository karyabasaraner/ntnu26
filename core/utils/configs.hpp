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

struct V4L2SettingConfig {
    int value;
    std::string name;
    uint32_t id{0};
};

struct WriterConfig {
    size_t height{0};
    size_t width{0};
    std::vector<TransformConfig> transforms;
};

enum class CameraBackend : uint8_t {
    none,
    v4l2,
    pylon,
};

struct V4L2CameraConfig {
    size_t req_buffer_count{4};
    std::string device;
    std::string format;
    std::vector<V4L2SettingConfig> settings;
};

struct PylonCameraConfig {
    double exposure_time_us{0.0};
    double gain{0.0};
    std::string format;
    std::string ip_address;
    uint32_t inter_packet_delay{0};
    uint32_t packet_size{0};
};

struct CameraConfig {
    CameraBackend backend{CameraBackend::none};
    size_t fps{0};
    std::string name;
    PylonCameraConfig pylon;
    uint32_t subsample_factor{1};
    V4L2CameraConfig v4l2;
    WriterConfig writer;
};

enum class EventCameraBackend : uint8_t {
    none,
    prophesee,
};

struct PropheseeCameraConfig {
    std::string bias_file;    // Optional path to a Metavision .bias file; empty keeps sensor defaults
    std::string serial_number; // Empty selects the first available Prophesee camera
};

struct EventCameraConfig {
    EventCameraBackend backend{EventCameraBackend::none};
    size_t height{0};
    // Capacity of one published shared-memory frame, in events. A single acquisition
    // batch larger than this is split across multiple frames; no events are dropped.
    uint32_t max_events_per_frame{0};
    std::string name;
    PropheseeCameraConfig prophesee;
    size_t width{0};
};

struct IMUConfig {
    float sampling_frequency;
    float scale;
    size_t buffer_samples;
    size_t watermark_samples;
    std::string device;
    std::string name;
    std::vector<std::string> channels;
    WriterConfig writer;
};

struct RootConfig {
    std::vector<CameraConfig> cameras;
    std::vector<EventCameraConfig> event_cameras;
    std::vector<IMUConfig> imus;
    std::vector<RingBufferConfig> shared_memory;
};

void declare_config(CameraConfig& config);
void declare_config(EventCameraConfig& config);
void declare_config(IMUConfig& config);
void declare_config(PropheseeCameraConfig& config);
void declare_config(PylonCameraConfig& config);
void declare_config(RootConfig& config);
void declare_config(V4L2CameraConfig& config);
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
