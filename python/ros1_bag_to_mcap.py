#!/usr/bin/env python3
from __future__ import annotations

import argparse
import base64
import importlib
import json
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Iterable, Protocol


JSON_SCHEMA_ENCODING = "jsonschema"
JSON_MESSAGE_ENCODING = "json"
COMPRESSED_IMAGE_SCHEMA_NAME = "foxglove.CompressedImage"
IMU_SCHEMA_NAME = "core.ImuXYZ"

COMPRESSED_IMAGE_SCHEMA = b"""{
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
"""

IMU_SCHEMA = b"""{
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
"""

NANOSECONDS_PER_SECOND = 1_000_000_000


class RosbagReaderProtocol(Protocol):
    connections: list[Any]

    def __enter__(self) -> "RosbagReaderProtocol": ...

    def __exit__(self, exc_type, exc, tb) -> None: ...

    def messages(self, *args: Any, **kwargs: Any) -> Iterable[tuple[Any, int, bytes]]: ...

    def deserialize(self, rawdata: bytes, msgtype: str) -> Any: ...


class McapWriterProtocol(Protocol):
    def register_schema(self, name: str, encoding: str, data: bytes) -> Any: ...

    def register_channel(self, topic: str, message_encoding: str, schema_id: Any) -> Any: ...

    def add_message(self, channel_id: Any, log_time: int, data: bytes, publish_time: int, sequence: int) -> None: ...

    def finish(self) -> None: ...


@dataclass(frozen=True, slots=True)
class InferredCameraTopic:
    source_topic: str
    output_topic: str


@dataclass(frozen=True, slots=True)
class InferredImuTopic:
    source_topic: str
    accel_topic: str
    gyro_topic: str


def _ensure_module(module_name: str) -> Any:
    try:
        return importlib.import_module(module_name)
    except ModuleNotFoundError as exc:
        raise ModuleNotFoundError(
            f"Missing Python dependency '{module_name}'. Install it before running the converter."
        ) from exc


def _normalize_msgtype_name(msgtype: str) -> str:
    return msgtype.replace("/msg/", "/")


def _topic_timestamp_ns(message: Any) -> int:
    header = getattr(message, "header", None)
    if header is None:
        raise ValueError("ROS message is missing header")

    stamp = getattr(header, "stamp", None)
    if stamp is None:
        raise ValueError("ROS message header is missing stamp")

    sec = getattr(stamp, "sec", None)
    if sec is None:
        sec = getattr(stamp, "secs", None)
    nsec = getattr(stamp, "nanosec", None)
    if nsec is None:
        nsec = getattr(stamp, "nsecs", None)
    if sec is None or nsec is None:
        raise ValueError("ROS message timestamp does not expose sec/nsec fields")
    return int(sec) * NANOSECONDS_PER_SECOND + int(nsec)


def _topic_frame_id(message: Any) -> str:
    header = getattr(message, "header", None)
    if header is None:
        raise ValueError("ROS message is missing header")
    frame_id = getattr(header, "frame_id", "")
    return str(frame_id)


def _message_sequence(message: Any) -> int:
    header = getattr(message, "header", None)
    if header is None:
        raise ValueError("ROS message is missing header")

    sequence = getattr(header, "seq", None)
    if sequence is None:
        sequence = getattr(header, "sequence", None)
    if sequence is None:
        raise ValueError("ROS message header is missing sequence")
    return int(sequence)


def _seconds_and_nanoseconds(timestamp_ns: int) -> dict[str, int]:
    return {
        "sec": timestamp_ns // NANOSECONDS_PER_SECOND,
        "nsec": timestamp_ns % NANOSECONDS_PER_SECOND,
    }


def _json_payload(body: dict[str, Any]) -> bytes:
    return json.dumps(body, separators=(",", ":"), ensure_ascii=False).encode("utf-8")


def build_compressed_image_payload(timestamp_ns: int, frame_id: str, image_format: str, data: bytes) -> bytes:
    return _json_payload(
        {
            "timestamp": _seconds_and_nanoseconds(timestamp_ns),
            "frame_id": frame_id,
            "data": base64.b64encode(data).decode("ascii"),
            "format": image_format,
        }
    )


def build_imu_payload(timestamp_ns: int, sequence: int, x: float, y: float, z: float) -> bytes:
    return _json_payload(
        {
            "timestamp": _seconds_and_nanoseconds(timestamp_ns),
            "sequence": int(sequence),
            "x": float(x),
            "y": float(y),
            "z": float(z),
        }
    )


def _open_rosbag_reader(input_path: Path) -> Any:
    rosbags_highlevel = _ensure_module("rosbags.highlevel")
    any_reader = getattr(rosbags_highlevel, "AnyReader", None)
    if any_reader is None:
        raise RuntimeError("rosbags.highlevel.AnyReader is unavailable in this rosbags version")
    return any_reader([input_path])


def _open_mcap_writer(output_path: Path) -> Any:
    mcap_writer_module = _ensure_module("mcap.writer")
    writer_cls = getattr(mcap_writer_module, "Writer", None)
    if writer_cls is None:
        raise RuntimeError("mcap.writer.Writer is unavailable")
    output_path.parent.mkdir(parents=True, exist_ok=True)
    handle = output_path.open("wb")
    writer = writer_cls(handle)
    writer_start = getattr(writer, "start", None)
    if callable(writer_start):
        writer_start()
    return writer, handle


def _register_core_schemas(writer: McapWriterProtocol) -> dict[str, Any]:
    schemas = {
        COMPRESSED_IMAGE_SCHEMA_NAME: writer.register_schema(
            COMPRESSED_IMAGE_SCHEMA_NAME,
            JSON_SCHEMA_ENCODING,
            bytes(COMPRESSED_IMAGE_SCHEMA),
        ),
        IMU_SCHEMA_NAME: writer.register_schema(
            IMU_SCHEMA_NAME,
            JSON_SCHEMA_ENCODING,
            bytes(IMU_SCHEMA),
        ),
    }
    return schemas


def _normalize_topic(topic: str) -> str:
    normalized = topic.strip()
    if not normalized.startswith("/"):
        normalized = "/" + normalized
    while normalized.endswith("/") and normalized != "/":
        normalized = normalized[:-1]
    return normalized


def _infer_camera_output_topic(source_topic: str) -> str:
    normalized_source_topic = _normalize_topic(source_topic)
    if normalized_source_topic.endswith("/image/compressed"):
        topic_root = normalized_source_topic[: -len("/image/compressed")].strip("/")
        if not topic_root:
            raise ValueError(f"Camera topic '{source_topic}' does not contain a camera name")

        if topic_root.startswith("camera/"):
            return f"/{topic_root}/image/compressed"
        return f"/camera/{topic_root}/image/compressed"

    if normalized_source_topic.endswith("/compressed"):
        topic_root = normalized_source_topic[: -len("/compressed")].strip("/")
        if not topic_root:
            raise ValueError(f"Camera topic '{source_topic}' does not contain a camera name")

        camera_name = topic_root.rsplit("/", 1)[-1]
        return f"/camera/{camera_name}/image/compressed"

    raise ValueError(f"Camera topic '{source_topic}' must end with '/compressed'")


def _is_camera_source_topic(source_topic: str) -> bool:
    return _normalize_topic(source_topic).endswith("/compressed")


def _infer_imu_output_prefix(source_topic: str) -> str:
    normalized_source_topic = _normalize_topic(source_topic)
    if normalized_source_topic.endswith("/imu"):
        return "/imu"

    for suffix in ("/data_raw", "/data"):
        if normalized_source_topic.endswith(suffix):
            normalized_source_topic = normalized_source_topic[: -len(suffix)]
            break

    normalized_source_topic = normalized_source_topic.rstrip("/")
    if not normalized_source_topic:
        return "/imu"
    return normalized_source_topic


def _is_imu_source_topic(source_topic: str) -> bool:
    normalized_source_topic = _normalize_topic(source_topic)
    return normalized_source_topic.endswith("/data") or normalized_source_topic.endswith("/data_raw") or normalized_source_topic.endswith("/imu")


def _infer_camera_topics(connections: list[Any]) -> list[InferredCameraTopic]:
    inferred_topics: list[InferredCameraTopic] = []
    seen_outputs: dict[str, str] = {}
    for connection in connections:
        source_topic = str(getattr(connection, "topic", ""))
        if not _is_camera_source_topic(source_topic):
            continue

        msgtype = _normalize_msgtype_name(str(getattr(connection, "msgtype", "")))
        if msgtype != "sensor_msgs/CompressedImage":
            raise ValueError(f"Camera topic '{source_topic}' must contain sensor_msgs/CompressedImage, got '{connection.msgtype}'")

        output_topic = _infer_camera_output_topic(source_topic)
        previous_source = seen_outputs.get(output_topic)
        if previous_source is not None and previous_source != source_topic:
            raise ValueError(
                f"Camera topics '{previous_source}' and '{source_topic}' both infer the same output topic '{output_topic}'"
            )
        if previous_source is None:
            seen_outputs[output_topic] = source_topic
            inferred_topics.append(InferredCameraTopic(source_topic=source_topic, output_topic=output_topic))
    return inferred_topics


def _infer_imu_topics(connections: list[Any]) -> list[InferredImuTopic]:
    inferred_topics: list[InferredImuTopic] = []
    seen_outputs: dict[tuple[str, str], str] = {}
    for connection in connections:
        source_topic = str(getattr(connection, "topic", ""))
        if not _is_imu_source_topic(source_topic):
            continue

        msgtype = _normalize_msgtype_name(str(getattr(connection, "msgtype", "")))
        if msgtype != "sensor_msgs/Imu":
            raise ValueError(f"IMU topic '{source_topic}' must contain sensor_msgs/Imu, got '{connection.msgtype}'")

        output_prefix = _infer_imu_output_prefix(source_topic)
        accel_topic = f"{output_prefix}/accelerometer"
        gyro_topic = f"{output_prefix}/gyroscope"
        output_key = (accel_topic, gyro_topic)
        previous_source = seen_outputs.get(output_key)
        if previous_source is not None and previous_source != source_topic:
            raise ValueError(
                f"IMU topics '{previous_source}' and '{source_topic}' both infer the same output prefix '{output_prefix}'"
            )
        if previous_source is None:
            seen_outputs[output_key] = source_topic
            inferred_topics.append(
                InferredImuTopic(source_topic=source_topic, accel_topic=accel_topic, gyro_topic=gyro_topic)
            )
    return inferred_topics


def _register_core_channels(
    writer: McapWriterProtocol,
    camera_topics: list[InferredCameraTopic],
    imu_topics: list[InferredImuTopic],
) -> dict[str, Any]:
    schemas = _register_core_schemas(writer)
    channels: dict[str, Any] = {}
    for topic in camera_topics:
        channels[topic.output_topic] = writer.register_channel(
            topic.output_topic,
            JSON_MESSAGE_ENCODING,
            schemas[COMPRESSED_IMAGE_SCHEMA_NAME],
        )
    for topic in imu_topics:
        channels[topic.accel_topic] = writer.register_channel(
            topic.accel_topic,
            JSON_MESSAGE_ENCODING,
            schemas[IMU_SCHEMA_NAME],
        )
        channels[topic.gyro_topic] = writer.register_channel(
            topic.gyro_topic,
            JSON_MESSAGE_ENCODING,
            schemas[IMU_SCHEMA_NAME],
        )
    return channels


def _deserialize_message(reader: Any, connection: Any, rawdata: bytes) -> Any:
    if hasattr(reader, "deserialize"):
        return reader.deserialize(rawdata, connection.msgtype)
    raise RuntimeError("rosbags reader does not provide a deserialize method")


def _write_camera_message(writer: McapWriterProtocol, channel_id: Any, timestamp_ns: int, payload: bytes) -> None:
    writer.add_message(channel_id, timestamp_ns, payload, timestamp_ns, 0)


def _write_imu_message(writer: McapWriterProtocol, channel_id: Any, timestamp_ns: int, sequence: int, payload: bytes) -> None:
    writer.add_message(channel_id, timestamp_ns, payload, timestamp_ns, sequence)


def _validate_sensor_topics_were_inferred(
    connections: list[Any],
    camera_topics: list[InferredCameraTopic],
    imu_topics: list[InferredImuTopic],
) -> None:
    inferred_topics = {topic.source_topic for topic in camera_topics} | {topic.source_topic for topic in imu_topics}
    skipped_sensor_topics: list[str] = []

    for connection in connections:
        topic = str(getattr(connection, "topic", ""))
        normalized_msgtype = _normalize_msgtype_name(str(getattr(connection, "msgtype", "")))
        if normalized_msgtype not in {"sensor_msgs/CompressedImage", "sensor_msgs/Imu"}:
            continue
        if topic in inferred_topics:
            continue

        skipped_sensor_topics.append(f"{topic} ({connection.msgtype})")

    if skipped_sensor_topics:
        skipped_topics = ", ".join(skipped_sensor_topics)
        raise ValueError(f"Sensor topics were not inferred and would be dropped: {skipped_topics}")


def convert_ros1_bag_to_mcap(
    input_path: str | Path,
    output_path: str | Path,
) -> None:
    input_path = Path(input_path)
    output_path = Path(output_path)
    writer, handle = _open_mcap_writer(output_path)

    try:
        with _open_rosbag_reader(input_path) as reader:
            connections = list(getattr(reader, "connections", []))
            if not connections:
                raise ValueError(f"No connections were found in rosbag '{input_path}'")

            camera_topics = _infer_camera_topics(connections)
            imu_topics = _infer_imu_topics(connections)
            _validate_sensor_topics_were_inferred(connections, camera_topics, imu_topics)
            channel_lookup = _register_core_channels(writer, camera_topics, imu_topics)
            camera_topic_lookup = {topic.source_topic: topic for topic in camera_topics}
            imu_topic_lookup = {topic.source_topic: topic for topic in imu_topics}

            for connection, _timestamp, rawdata in reader.messages():
                normalized_msgtype = _normalize_msgtype_name(str(getattr(connection, "msgtype", "")))
                topic = str(getattr(connection, "topic", ""))
                message = _deserialize_message(reader, connection, rawdata)

                if topic in camera_topic_lookup:
                    if normalized_msgtype != "sensor_msgs/CompressedImage":
                        raise ValueError(
                            f"Camera topic '{topic}' must contain sensor_msgs/CompressedImage, got '{connection.msgtype}'"
                        )
                    timestamp_ns = _topic_timestamp_ns(message)
                    frame_id = _topic_frame_id(message)
                    image_format = str(getattr(message, "format", ""))
                    data = bytes(getattr(message, "data", b""))
                    payload = build_compressed_image_payload(timestamp_ns, frame_id, image_format, data)
                    _write_camera_message(writer, channel_lookup[camera_topic_lookup[topic].output_topic], timestamp_ns, payload)
                    continue

                if topic in imu_topic_lookup:
                    if normalized_msgtype != "sensor_msgs/Imu":
                        raise ValueError(
                            f"IMU topic '{topic}' must contain sensor_msgs/Imu, got '{connection.msgtype}'"
                        )
                    timestamp_ns = _topic_timestamp_ns(message)
                    sequence = _message_sequence(message)
                    linear = getattr(message, "linear_acceleration", None)
                    angular = getattr(message, "angular_velocity", None)
                    if linear is None or angular is None:
                        raise ValueError(f"IMU topic '{topic}' is missing linear_acceleration or angular_velocity")

                    accel_payload = build_imu_payload(
                        timestamp_ns,
                        sequence,
                        getattr(linear, "x"),
                        getattr(linear, "y"),
                        getattr(linear, "z"),
                    )
                    gyro_payload = build_imu_payload(
                        timestamp_ns,
                        sequence,
                        getattr(angular, "x"),
                        getattr(angular, "y"),
                        getattr(angular, "z"),
                    )
                    imu_topic = imu_topic_lookup[topic]
                    _write_imu_message(writer, channel_lookup[imu_topic.accel_topic], timestamp_ns, sequence, accel_payload)
                    _write_imu_message(writer, channel_lookup[imu_topic.gyro_topic], timestamp_ns, sequence, gyro_payload)
                    continue

        writer.finish()
    finally:
        handle.close()


def _build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Convert a ROS1 bag into a core MCAP file by inferring camera and IMU topics")
    parser.add_argument("input_bag", help="Path to the ROS1 bag")
    parser.add_argument("output_mcap", help="Path to the output MCAP file")
    return parser


def main(argv: list[str] | None = None) -> int:
    parser = _build_parser()
    args = parser.parse_args(argv)
    convert_ros1_bag_to_mcap(args.input_bag, args.output_mcap)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
