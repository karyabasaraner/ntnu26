#ifndef WORKSPACES_CORE_CORE_MODULES_SHARED_MEMORY_LOGGER_SCHEMA_HPP
#define WORKSPACES_CORE_CORE_MODULES_SHARED_MEMORY_LOGGER_SCHEMA_HPP

#include <array>

namespace core {

constexpr auto JsonSchemaEncoding = "jsonschema";
constexpr auto JsonMessageEncoding = "json";
constexpr auto CompressedImageSchemaName = "foxglove.CompressedImage";
constexpr auto ImuSchemaName = "core.ImuXYZ";

constexpr auto CompressedImageSchema = std::to_array(
    R"({
  "title": "foxglove.CompressedImage",
  "description": "A compressed image",
  "type": "object",
  "properties": {
    "timestamp": {
      "type": "object",
      "properties": {
        "sec": { "type": "integer", "minimum": 0 },
        "nsec": { "type": "integer", "minimum": 0 }
      },
      "required": [ "sec", "nsec" ]
    },
    "frame_id": { "type": "string" },
    "data": { "type": "string", "contentEncoding": "base64" },
    "format": { "type": "string" }
  },
  "required": [ "timestamp", "frame_id", "data", "format" ]
}
)"
);

constexpr auto ImuSchema = std::to_array(
    R"({
  "title": "core.ImuXYZ",
  "description": "A 3-axis IMU sample",
  "type": "object",
  "properties": {
    "timestamp": {
      "type": "object",
      "properties": {
        "sec": { "type": "integer", "minimum": 0 },
        "nsec": { "type": "integer", "minimum": 0 }
      },
      "required": [ "sec", "nsec" ]
    },
    "sequence": { "type": "integer", "minimum": 0 },
    "x": { "type": "number" },
    "y": { "type": "number" },
    "z": { "type": "number" }
  },
  "required": [ "timestamp", "sequence", "x", "y", "z" ]
}
)"
);

} // namespace core
#endif // WORKSPACES_CORE_CORE_MODULES_SHARED_MEMORY_LOGGER_SCHEMA_HPP
