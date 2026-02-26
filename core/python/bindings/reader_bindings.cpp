#include <memory>
#include <stdexcept>
#include <string>

#include <nanobind/nanobind.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/tuple.h>
#include <nanobind/ndarray.h>

#include "core/utils/configs.hpp"
#include "core/modules/shared_memory/shared_dict_reader.hpp"

namespace nb = nanobind;
using namespace nb::literals;

namespace core_bindings {

// Helper to create a SharedDictReader from a config file and camera name
static std::unique_ptr<core::SharedDictReader> make_reader(const std::string& config_path,
                                                           const std::string& camera_name) {
    core::Config cfg;
    cfg.load(config_path);
    const auto& cameras = cfg.get_config().cameras;
    for (const auto& cam : cameras) {
        if (cam.name == camera_name) {
            return std::make_unique<core::SharedDictReader>(cam);
        }
    }
    throw std::runtime_error("Camera name not found in config: " + camera_name);
}

// Raw-pointer variant for nanobind with take_ownership policy
static core::SharedDictReader* make_reader_raw(const std::string& config_path,
                                               const std::string& camera_name) {
    core::Config cfg;
    cfg.load(config_path);
    const auto& cameras = cfg.get_config().cameras;
    for (const auto& cam : cameras) {
        if (cam.name == camera_name) {
            return new core::SharedDictReader(cam);
        }
    }
    throw std::runtime_error("Camera name not found in config: " + camera_name);
}

} // namespace core_bindings

NB_MODULE(core_sharedmem, m) {
    m.doc() = "nanobind bindings for reading shared memory frames";

    // Expose the class without a Python-side __init__; construct via factory below
    nb::class_<core::SharedDictReader>(m, "SharedDictReader")
        .def("is_ready", &core::SharedDictReader::is_ready,
             "Return True if the shared memory mapping is ready")
        .def("read", [](core::SharedDictReader& self, uint32_t index_from_head) {
                core::DataEntry entry{};
                {
                    nb::gil_scoped_release rel;
                    self.read(entry, index_from_head);
                }
                // Zero-copy numpy view over an owning vector using a capsule owner
                auto vec_holder = std::make_shared<std::vector<uint8_t>>(std::move(entry.data));
                uint8_t* ptr = vec_holder->data();
                const size_t n = vec_holder->size();

                // Keep the vector alive via capsule ownership
                auto owner = nb::capsule(new std::shared_ptr<std::vector<uint8_t>>(vec_holder),
                                         [](void* p) noexcept {
                                             delete reinterpret_cast<std::shared_ptr<std::vector<uint8_t>>*>(p);
                                         });

                // 1-D contiguous array: omit explicit strides so nanobind infers C-contiguous layout
                nb::ndarray<nb::numpy, uint8_t> arr(ptr, {n}, owner);
                return nb::make_tuple(entry.key, std::move(arr), entry.sequence, entry.timestamp_ns);
            },
            "index_from_head"_a = 0,
            "Read a frame; returns (key, np.ndarray[uint8], sequence, timestamp_ns). Array is 1-D; reshape as needed.");

    // Python factory: returns an owning instance (std::unique_ptr) of SharedDictReader
    m.def("make_reader", &core_bindings::make_reader_raw, nb::rv_policy::take_ownership,
          "config_path"_a, "camera_name"_a,
          "Create a SharedDictReader from a YAML config and camera name");
}
