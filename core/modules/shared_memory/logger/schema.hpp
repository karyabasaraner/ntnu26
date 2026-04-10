#ifndef WORKSPACES_CORE_CORE_MODULES_SHARED_MEMORY_LOGGER_SCHEMA_HPP
#define WORKSPACES_CORE_CORE_MODULES_SHARED_MEMORY_LOGGER_SCHEMA_HPP

#include <array>

namespace core {

constexpr auto CompressedImageSchema = std::to_array(
    "uint64 source_timestamp_ns\n"
    "uint32 source_sequence\n"
    "uint32 width\n"
    "uint32 height\n"
    "uint8 channels\n"
    "uint8 jpeg_quality\n"
    "bytes jpeg_data\n"
);

constexpr auto ImuSchema = std::to_array(
    "uint64 source_timestamp_ns\n"
    "uint32 source_sequence\n"
    "float32 x\n"
    "float32 y\n"
    "float32 z\n"
);

} // namespace core
#endif // WORKSPACES_CORE_CORE_MODULES_SHARED_MEMORY_LOGGER_SCHEMA_HPP
