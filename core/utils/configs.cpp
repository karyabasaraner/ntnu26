#include <config_utilities/config.h>
#include <config_utilities/parsing/yaml.h>
#include <iostream>
#include <string>

#include "configs.hpp"

namespace core {

void declare_config(RingBufferConfig& config) {
    config::name("RingBufferConfig");
    config::field(config.name, "name", "Name of the ring buffer");
    config::field(config.size_per_frame, "size_per_frame", "Size of each frame in bytes");
    config::field(config.num_frames, "num_frames", "Number of frames in the ring buffer");
}

void declare_config(SharedMemoryConfig& config) {
    config::name("SharedMemoryConfig");
    config::field(config.ringbuffers, "ringbuffers", "List of ring buffer configurations");
}

void declare_config(TransformConfig& config) {
    config::name("TransformConfig");
    config::field(config.name, "name", "Name of the transform to apply");
}

void declare_config(CameraConfig& config) {
    config::name("CameraConfig");
    config::field(config.device, "device", "Device path of the camera");
    config::field(config.format, "format", "Pixel format (e.g., RGB24)");
    config::field(config.fps, "fps", "Frames per second");
    config::field(config.height, "height", "Image height in pixels");
    config::field(config.name, "name", "Name of the camera");
    config::field(config.req_buffer_count, "req_buffer_count", "Number of buffers to request for memory mapping");
    config::field(config.width, "width", "Image width in pixels");
    config::field(config.transforms, "transforms", "List of transforms to apply sequentially");
}

void declare_config(RootConfig& config) {
    config::name("RootConfig");
    config::field(config.cameras, "cameras", "List of camera configurations");
    config::field(config.shared_memory, "shared_memory", "Shared memory configuration");
}

void Config::load(const std::string& config_path) {
    _config = config::fromYamlFile<RootConfig>(config_path);
    std::cout << "Config loaded from: " << config_path << '\n';
}

const RootConfig& Config::get_config() {
    return _config;
}

} // namespace core
