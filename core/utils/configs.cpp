#include <config_utilities/config.h>
#include <config_utilities/parsing/yaml.h>
#include <config_utilities/validation.h>
#include <cstddef>
#include <iostream>
#include <string>

#include "configs.hpp"

namespace core {

void declare_config(RingBufferConfig& config) {
    config::name("RingBufferConfig");
    config::field(config.name, "name", "Name of the ring buffer");
    config::field(config.size_per_frame, "size_per_frame", "Size of each frame in bytes");
    config::field(config.num_frames, "num_frames", "Number of frames in the ring buffer");
    config::check(config.name.size(), config::CheckMode::GT, static_cast<std::size_t>(0), "name");
    config::check(config.size_per_frame, config::CheckMode::GT, static_cast<std::size_t>(0), "size_per_frame");
    config::check(config.num_frames, config::CheckMode::GT, static_cast<std::size_t>(0), "num_frames");
}

void declare_config(TransformConfig& config) {
    config::name("TransformConfig");
    config::field(config.name, "name", "Name of the transform to apply");
    config::check(config.name.size(), config::CheckMode::GT, static_cast<std::size_t>(0), "name");
}

void declare_config(SettingsConfig& config) {
    config::name("SettingsConfig");
    config::field(config.id, "id", "Optional V4L2 control id (hex or decimal), e.g., 0x00980913");
    config::field(config.name, "name", "Name of the setting to apply");
    config::field(config.value, "value", "Value of the setting to apply");
    config::check(config.name.size(), config::CheckMode::GT, static_cast<std::size_t>(0), "name");
}

void declare_config(WriterConfig& config) {
    config::name("WriterConfig");
    config::field(config.width, "width", "Width of the frames to write");
    config::field(config.height, "height", "Height of the frames to write");
    config::field(config.transforms, "transforms", "List of transforms to apply sequentially");
}

void declare_config(CameraConfig& config) {
    config::name("CameraConfig");
    config::field(config.device, "device", "Device path of the camera");
    config::field(config.format, "format", "Pixel format (e.g., RGB24)");
    config::field(config.fps, "fps", "Frames per second");
    config::field(config.name, "name", "Name of the camera");
    config::field(config.req_buffer_count, "req_buffer_count", "Number of buffers to request for memory mapping");
    config::field(config.settings, "settings", "List of key-value settings for the camera (e.g., vertical_flip)");
    config::field(config.subsample_factor, "subsample_factor", "Factor by which to subsample frames (e.g., 4 means keep 1 in every 4 frames)");
    config::field(config.writer, "writer");
    config::check(config.device.size(), config::CheckMode::GT, static_cast<std::size_t>(0), "device");
    config::check(config.format.size(), config::CheckMode::GT, static_cast<std::size_t>(0), "format");
    config::check(config.name.size(), config::CheckMode::GT, static_cast<std::size_t>(0), "name");
    config::check(config.fps, config::CheckMode::GT, static_cast<std::size_t>(0), "fps");
    config::check(config.req_buffer_count, config::CheckMode::GT, static_cast<std::size_t>(0), "req_buffer_count");
    config::check(config.subsample_factor, config::CheckMode::GT, 0U, "subsample_factor");
    config::check(config.writer.width, config::CheckMode::GT, static_cast<std::size_t>(0), "writer.width");
    config::check(config.writer.height, config::CheckMode::GT, static_cast<std::size_t>(0), "writer.height");
}

void declare_config(IMUConfig& config) {
    config::name("IMUConfig");
    config::field(config.writer, "writer");
    config::field(config.name, "name", "Name of the IMU");
    config::field(config.device, "device", "IIO device identifier (e.g., iio:device0)");
    config::field(config.sampling_frequency, "sampling_frequency", "Target sampling frequency in Hz");
    config::field(config.scale, "scale", "Scale factor for the IMU readings");
    config::field(config.buffer_samples, "buffer_samples", "Number of samples in the kernel IIO buffer");
    config::field(config.watermark_samples, "watermark_samples", "Number of samples to trigger buffer watermark interrupt");
    config::field(config.channels, "channels", "List of channel IDs to enable for each sample");
    config::check(config.name.size(), config::CheckMode::GT, static_cast<std::size_t>(0), "name");
    config::check(config.device.size(), config::CheckMode::GT, static_cast<std::size_t>(0), "device");
    config::check(config.sampling_frequency, config::CheckMode::GT, 0.0F, "sampling_frequency");
    config::check(config.scale, config::CheckMode::GT, 0.0F, "scale");
    config::check(config.buffer_samples, config::CheckMode::GT, static_cast<std::size_t>(0), "buffer_samples");
    config::check(config.watermark_samples, config::CheckMode::GT, static_cast<std::size_t>(0), "watermark_samples");
    config::check(config.channels.size(), config::CheckMode::GT, static_cast<std::size_t>(0), "channels");
}

void declare_config(RootConfig& config) {
    config::name("RootConfig");
    config::field(config.cameras, "cameras", "List of camera configurations");
    config::field(config.imus, "imus", "List of IMU configurations");
    config::field(config.shared_memory, "shared_memory", "List of shared memory configurations");
}

void Config::load(const std::string& config_path) {
    _config = config::checkValid(config::fromYamlFile<RootConfig>(config_path));
    std::cout << "Config loaded from: " << config_path << '\n';
}

const RootConfig& Config::get_config() {
    return _config;
}

} // namespace core
