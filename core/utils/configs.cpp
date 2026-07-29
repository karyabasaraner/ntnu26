#include <config_utilities/config.h>
#include <config_utilities/parsing/yaml.h>
#include <config_utilities/validation.h>
#include <cstddef>
#include <iostream>
#include <stdexcept>
#include <string>
#include <yaml-cpp/node/node.h>
#include <yaml-cpp/node/parse.h>

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

void declare_config(V4L2SettingConfig& config) {
    config::name("V4L2SettingConfig");
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

void declare_config(V4L2CameraConfig& config) {
    config::name("V4L2CameraConfig");
    config::field(config.device, "device", "V4L2 device path");
    config::field(config.format, "format", "V4L2 pixel format (e.g., UYVY)");
    config::field(config.req_buffer_count, "req_buffer_count", "Number of V4L2 buffers to request");
    config::field(config.settings, "settings", "List of V4L2 controls to apply");
    config::check(config.req_buffer_count, config::CheckMode::GT, static_cast<std::size_t>(0), "req_buffer_count");
}

void declare_config(PylonCameraConfig& config) {
    config::name("PylonCameraConfig");
    config::field(config.exposure_time_us, "exposure_time_us", "Manual exposure time in microseconds; 0 keeps the camera default");
    config::field(config.format, "format", "GenICam pixel format (e.g., BayerRG8)");
    config::field(config.gain, "gain", "Manual camera gain; 0 keeps the camera default");
    config::field(config.inter_packet_delay, "inter_packet_delay", "GigE inter-packet delay in camera ticks; 0 keeps the camera default");
    config::field(config.ip_address, "ip_address", "Static IP address of the GigE camera");
    config::field(config.packet_size, "packet_size", "GigE stream packet size in bytes; 0 keeps the camera default");
}

void declare_config(CameraConfig& config) {
    config::name("CameraConfig");
    config::field(config.fps, "fps", "Frames per second");
    config::field(config.name, "name", "Name of the camera");
    config::field(config.pylon, "pylon");
    config::field(config.subsample_factor, "subsample_factor", "Factor by which to subsample frames (e.g., 4 means keep 1 in every 4 frames)");
    config::field(config.v4l2, "v4l2");
    config::field(config.writer, "writer");
    config::checkCondition(config.backend != CameraBackend::none, "Camera must contain exactly one 'v4l2' or 'pylon' configuration");
    if (config.backend == CameraBackend::v4l2) {
        config::check(config.v4l2.device.size(), config::CheckMode::GT, static_cast<std::size_t>(0), "v4l2.device");
        config::check(config.v4l2.format.size(), config::CheckMode::GT, static_cast<std::size_t>(0), "v4l2.format");
    }
    if (config.backend == CameraBackend::pylon) {
        config::check(config.pylon.ip_address.size(), config::CheckMode::GT, static_cast<std::size_t>(0), "pylon.ip_address");
        config::check(config.pylon.format.size(), config::CheckMode::GT, static_cast<std::size_t>(0), "pylon.format");
    }
    config::check(config.name.size(), config::CheckMode::GT, static_cast<std::size_t>(0), "name");
    config::check(config.fps, config::CheckMode::GT, static_cast<std::size_t>(0), "fps");
    config::check(config.subsample_factor, config::CheckMode::GT, 0U, "subsample_factor");
    config::check(config.writer.width, config::CheckMode::GT, static_cast<std::size_t>(0), "writer.width");
    config::check(config.writer.height, config::CheckMode::GT, static_cast<std::size_t>(0), "writer.height");
}

void declare_config(PropheseeCameraConfig& config) {
    config::name("PropheseeCameraConfig");
    config::field(config.bias_file, "bias_file", "Optional path to a Metavision .bias file; empty keeps sensor defaults");
    config::field(config.serial_number, "serial_number", "Serial number of the Prophesee camera; empty selects the first available one");
}

void declare_config(EventCameraConfig& config) {
    config::name("EventCameraConfig");
    config::field(config.height, "height", "Sensor height in pixels");
    config::field(config.max_events_per_frame, "max_events_per_frame", "Maximum number of events per published shared-memory frame");
    config::field(config.name, "name", "Name of the event camera");
    config::field(config.prophesee, "prophesee");
    config::field(config.width, "width", "Sensor width in pixels");
    config::check(config.name.size(), config::CheckMode::GT, static_cast<std::size_t>(0), "name");
    config::check(config.width, config::CheckMode::GT, static_cast<std::size_t>(0), "width");
    config::check(config.height, config::CheckMode::GT, static_cast<std::size_t>(0), "height");
    config::check(config.max_events_per_frame, config::CheckMode::GT, 0U, "max_events_per_frame");
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
    config::field(config.event_cameras, "event_cameras", "List of event camera configurations");
    config::field(config.imus, "imus", "List of IMU configurations");
    config::field(config.shared_memory, "shared_memory", "List of shared memory configurations");
}

void Config::load(const std::string& config_path) {
    const YAML::Node root_node = YAML::LoadFile(config_path);
    _config = config::fromYaml<RootConfig>(root_node);

    const YAML::Node cameras_node = root_node["cameras"];
    if (!cameras_node || !cameras_node.IsSequence() || cameras_node.size() != _config.cameras.size()) {
        throw std::runtime_error("The 'cameras' field must be a sequence");
    }
    for (std::size_t index = 0; index < _config.cameras.size(); ++index) {
        const bool has_v4l2 = cameras_node[index]["v4l2"].IsDefined();
        const bool has_pylon = cameras_node[index]["pylon"].IsDefined();
        if (has_v4l2 == has_pylon) {
            throw std::runtime_error(
                "Camera '" + _config.cameras[index].name + "' must contain exactly one 'v4l2' or 'pylon' configuration"
            );
        }
        _config.cameras[index].backend = has_v4l2 ? CameraBackend::v4l2 : CameraBackend::pylon;
    }

    // 'event_cameras' is a newer, optional top-level field; configs that predate it
    // simply omit the key and get an empty list.
    const YAML::Node event_cameras_node = root_node["event_cameras"];
    if (event_cameras_node) {
        if (!event_cameras_node.IsSequence() || event_cameras_node.size() != _config.event_cameras.size()) {
            throw std::runtime_error("The 'event_cameras' field must be a sequence");
        }
        for (std::size_t index = 0; index < _config.event_cameras.size(); ++index) {
            if (!event_cameras_node[index]["prophesee"].IsDefined()) {
                throw std::runtime_error(
                    "Event camera '" + _config.event_cameras[index].name + "' must contain a 'prophesee' configuration"
                );
            }
            _config.event_cameras[index].backend = EventCameraBackend::prophesee;
        }
    }

    _config = config::checkValid(_config);
    std::cout << "Config loaded from: " << config_path << '\n';
}

const RootConfig& Config::get_config() {
    return _config;
}

} // namespace core
