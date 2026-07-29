#ifndef WORKSPACES_CORE_CORE_MODULES_SHARED_MEMORY_LOGGER_SCHEMA_HPP
#define WORKSPACES_CORE_CORE_MODULES_SHARED_MEMORY_LOGGER_SCHEMA_HPP

#include <array>

namespace core {

constexpr auto JsonSchemaEncoding = "jsonschema";
constexpr auto JsonMessageEncoding = "json";
constexpr auto CompressedImageSchemaName = "foxglove.CompressedImage";
constexpr auto ImuSchemaName = "core.ImuXYZ";
constexpr auto EventArraySchemaName = "core.EventArray";

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

constexpr auto EventArraySchema = std::to_array(
    R"({
  "title": "core.EventArray",
  "description": "A batch of raw event-camera (change-detection) events, packed as little-endian records: int64 timestamp_ns, uint16 x, uint16 y, uint8 polarity, uint8 reserved (16 bytes each)",
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
    "num_events": { "type": "integer", "minimum": 0 },
    "encoding": { "type": "string" },
    "data": { "type": "string", "contentEncoding": "base64" }
  },
  "required": [ "timestamp", "sequence", "num_events", "encoding", "data" ]
}
)"
);

} // namespace core
#endif // WORKSPACES_CORE_CORE_MODULES_SHARED_MEMORY_LOGGER_SCHEMA_HPP
