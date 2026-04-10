#include <cstddef>
#include <cstdint>
#include <memory>
#include <spdlog/spdlog.h>
#include <string>
#include <utility>
#include <vector>

#include <nanobind/nanobind.h>
#include <nanobind/ndarray.h>
// NOLINTBEGIN(misc-include-cleaner): required for nanobind std::string/std::vector type casters.
#include <nanobind/stl/string.h>
#include <nanobind/stl/vector.h>
// NOLINTEND(misc-include-cleaner)

#include "core/modules/shared_memory/client/reader.hpp"
#include "core/modules/shared_memory/logger/log_reader.hpp"
#include "core/modules/shared_memory/utils.hpp"
#include "core/utils/configs.hpp"
#include "nanobind/nb_defs.h"

namespace nb = nanobind;

// NOLINTNEXTLINE(google-build-using-namespace)
using namespace nb::literals;

namespace {

// Helper to create a SharedDictReader from a config file and camera name
std::unique_ptr<core::SharedDictReader> make_reader(const std::string& config_path, const std::string& name) {
    core::Config cfg;
    cfg.load(config_path);
    const auto& cameras = cfg.get_config().cameras;
    for (const auto& cam : cameras) {
        if (cam.name == name) {
            spdlog::info("Creating SharedDictReader for camera: {}", name);
            return std::make_unique<core::SharedDictReader>(name);
        }
    }
    for (const auto& imu : cfg.get_config().imus) {
        if (imu.name == name) {
            spdlog::info("Creating SharedDictReader for IMU: {}", name);
            return std::make_unique<core::SharedDictReader>(name);
        }
    }
    spdlog::error("No camera or IMU with name '{}' found in config '{}'", name, config_path);
    return nullptr;
}

// Raw-pointer variant for nanobind with take_ownership policy)
core::SharedDictReader* make_reader_raw(nb::str& config_path, nb::str& name) {
    const std::string config_path_str = config_path.c_str();
    const std::string camera_name_str = name.c_str();
    auto reader = make_reader(config_path_str, camera_name_str);
    return reader.release();
}

template <typename T>
nb::ndarray<nb::numpy, T> vector_to_numpy(const std::vector<T>& values) {
    auto vec_holder = std::make_shared<std::vector<T>>(values);
    T* ptr = vec_holder->data();
    const size_t vec_holder_size = vec_holder->size();
    auto owner = nb::capsule(new std::shared_ptr<std::vector<T>>(vec_holder),
    [](void* pointer) noexcept
    {
        // NOLINTNEXTLINE(cppcoreguidelines-owning-memory)
        delete static_cast<std::shared_ptr<std::vector<T>>*>(pointer);
    });
    return nb::ndarray<nb::numpy, T>(ptr, {vec_holder_size}, owner);
}

const char* log_topic_type_to_string(core::LogTopicType type) {
    switch (type) {
        case core::LogTopicType::IMU:
            return "imu";
        case core::LogTopicType::COMPRESSED_IMAGE:
            return "compressed_image";
    }
    return "unknown";
}

nb::dict topic_metadata_to_dict(const core::TopicMetadata& metadata) {
    nb::dict result;
    result["topic"] = metadata.topic;
    result["type"] = log_topic_type_to_string(metadata.type);
    result["message_count"] = metadata.message_count;
    result["start_time_ns"] = metadata.start_time_ns;
    result["end_time_ns"] = metadata.end_time_ns;
    result["duration_s"] = metadata.duration_s;
    result["average_rate_hz"] = metadata.average_rate_hz;
    result["payload_bytes"] = metadata.payload_bytes;
    return result;
}

nb::dict log_metadata_to_dict(const core::LogMetadata& metadata) {
    nb::dict result;
    result["path"] = metadata.path;
    result["file_size_bytes"] = metadata.file_size_bytes;
    result["start_time_ns"] = metadata.start_time_ns;
    result["end_time_ns"] = metadata.end_time_ns;
    result["duration_s"] = metadata.duration_s;

    nb::dict topic_metadata;
    for (const auto& [topic, value] : metadata.topics) {
        topic_metadata[nb::str(topic.c_str())] = topic_metadata_to_dict(value);
    }
    result["topics"] = topic_metadata;
    return result;
}

nb::dict imu_series_to_dict(const core::LoggedImuSeries& series) {
    nb::dict result;
    result["topic"] = series.topic;
    result["timestamp_ns"] = vector_to_numpy(series.timestamp_ns);
    result["sequence"] = vector_to_numpy(series.sequence);
    result["x"] = vector_to_numpy(series.x);
    result["y"] = vector_to_numpy(series.y);
    result["z"] = vector_to_numpy(series.z);
    return result;
}

nb::dict camera_series_to_dict(const core::LoggedCompressedImageSeries& series) {
    nb::dict result;
    result["topic"] = series.topic;
    result["timestamp_ns"] = vector_to_numpy(series.timestamp_ns);
    result["frame_id"] = series.frame_id;
    result["format"] = series.format;

    nb::list jpeg_data;
    for (const auto& sample : series.jpeg_data) {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
        jpeg_data.append(nb::bytes(reinterpret_cast<const char*>(sample.data()), sample.size()));
    }
    result["jpeg_data"] = jpeg_data;
    return result;
}

} // namespace

NB_MODULE(core, module) {
    module.doc() = "nanobind bindings for reading shared memory frames and core MCAP logs";

    // Expose the class without a Python-side __init__; construct via factory below
    nb::class_<core::SharedDictReader>(module, "SharedDictReader")
        .def("is_ready", &core::SharedDictReader::is_ready, "Return True if the shared memory mapping is ready")

        .def("read", [](core::SharedDictReader& self) {
                core::DataEntry entry{};
                {
                    nb::gil_scoped_release const rel;
                    self.read_latest(entry);
                }

                // Zero-copy numpy view over an owning vector using a capsule owner
                auto vec_holder = std::make_shared<std::vector<uint8_t>>(std::move(entry.data));
                uint8_t* ptr = vec_holder->data();
                const size_t vec_holder_size = vec_holder->size();

                // Keep the vector alive via capsule ownership
                auto owner = nb::capsule(new std::shared_ptr<std::vector<uint8_t>>(vec_holder),
                [](void* pointer) noexcept
                {
                    // NOLINTNEXTLINE(cppcoreguidelines-owning-memory)
                    delete static_cast<std::shared_ptr<std::vector<uint8_t>>*>(pointer);
                });

                // 1-D contiguous array: omit explicit strides so nanobind infers C-contiguous layout
                nb::ndarray<nb::numpy, uint8_t> arr(ptr, {vec_holder_size}, owner);
                return nb::make_tuple(nb::str(entry.key.c_str()), entry.head, entry.sequence, entry.timestamp_ns, std::move(arr));
            },
            "Read a frame; returns (key, head, sequence, timestamp_ns, np.ndarray[uint8]). Array is 1-D; reshape as needed.");

    // Python factory: returns an owning instance (std::unique_ptr) of SharedDictReader
    module.def("make_reader", &make_reader_raw, nb::rv_policy::take_ownership,
        "config_path"_a, "camera_name"_a,
        "Create a SharedDictReader from a YAML config and camera name"
    );

    nb::class_<core::LogFile>(module, "LogFile")
        .def("topics", &core::LogFile::topics, "Return all supported topics available in the log")
        .def("imu_topics", &core::LogFile::imu_topics, "Return IMU topics available in the log")
        .def("camera_topics", &core::LogFile::camera_topics, "Return compressed camera topics available in the log")
        .def("metadata", [](const core::LogFile& self) {
            return log_metadata_to_dict(self.metadata());
        }, "Return file-level and per-topic metadata")
        .def("format_metadata", [](const core::LogFile& self) {
            return core::format_log_metadata(self.metadata());
        }, "Return printable metadata summary")
        .def("get_imu_data", [](const core::LogFile& self, const std::string& topic) -> nb::object {
            const auto* series = self.get_imu_data(topic);
            if (series == nullptr) {
                return nb::none();
            }
            return imu_series_to_dict(*series);
        }, "topic"_a, "Return an IMU topic as arrays, or None when unavailable")
        .def("get_camera_data", [](const core::LogFile& self, const std::string& topic) -> nb::object {
            const auto* series = self.get_camera_data(topic);
            if (series == nullptr) {
                return nb::none();
            }
            return camera_series_to_dict(*series);
        }, "topic"_a, "Return a compressed camera topic as arrays and JPEG bytes, or None when unavailable");

    module.def("read_log_file", [](const std::string& path) {
        nb::gil_scoped_release const rel;
        return core::read_log_file(path);
    }, "path"_a, "Read a core MCAP log file into memory");

    module.def("convert_legacy_log_file_to_foxglove", [](const std::string& input_path, const std::string& output_path) {
        nb::gil_scoped_release const rel;
        core::convert_legacy_log_file_to_foxglove(input_path, output_path);
    }, "input_path"_a, "output_path"_a, "Convert a legacy binary core MCAP log to the Foxglove-compatible JSON format");
}
