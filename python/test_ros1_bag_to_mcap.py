import json
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
import sys
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))

import ros1_bag_to_mcap


class _FakeWriter:
    def __init__(self) -> None:
        self.schemas = []
        self.channels = []
        self.messages = []
        self.finished = False
        self._schema_counter = 0
        self._channel_counter = 0

    def register_schema(self, name, encoding, data):
        self._schema_counter += 1
        schema_id = f"schema-{self._schema_counter}"
        self.schemas.append((schema_id, name, encoding, data))
        return schema_id

    def register_channel(self, topic, message_encoding, schema_id):
        self._channel_counter += 1
        channel_id = f"channel-{self._channel_counter}"
        self.channels.append((channel_id, topic, message_encoding, schema_id))
        return channel_id

    def add_message(self, channel_id, log_time, data, publish_time, sequence):
        self.messages.append((channel_id, log_time, data, publish_time, sequence))

    def finish(self):
        self.finished = True


class _FakeHandle:
    def __init__(self) -> None:
        self.closed = False

    def close(self) -> None:
        self.closed = True


class _FakeReader:
    def __init__(self, messages):
        self.connections = [message[0] for message in messages]
        self._messages = messages

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc, tb):
        return False

    def messages(self):
        for message in self._messages:
            yield message

    def deserialize(self, rawdata, msgtype):
        return rawdata


class Ros1BagToMcapTest(unittest.TestCase):
    def test_given_payload_helpers_when_values_are_encoded_then_json_matches_core_schema(self):
        # GIVEN: A deterministic image and IMU sample
        image_payload = ros1_bag_to_mcap.build_compressed_image_payload(1_234_567_890, "front", "jpeg", b"\x01\x02")
        imu_payload = ros1_bag_to_mcap.build_imu_payload(4_000_000_000, 17, 1.5, -2.25, 3.75)

        # WHEN: The payloads are decoded as JSON
        image_json = json.loads(image_payload.decode("utf-8"))
        imu_json = json.loads(imu_payload.decode("utf-8"))

        # THEN: The payloads match the stack's schema shape and preserve the values
        self.assertEqual(image_json["timestamp"], {"sec": 1, "nsec": 234_567_890})
        self.assertEqual(image_json["frame_id"], "front")
        self.assertEqual(image_json["data"], "AQI=")
        self.assertEqual(image_json["format"], "jpeg")
        self.assertEqual(imu_json["timestamp"], {"sec": 4, "nsec": 0})
        self.assertEqual(imu_json["sequence"], 17)
        self.assertAlmostEqual(imu_json["x"], 1.5)
        self.assertAlmostEqual(imu_json["y"], -2.25)
        self.assertAlmostEqual(imu_json["z"], 3.75)

    def test_given_source_topics_when_inferred_then_canonical_output_topics_are_returned(self):
        # GIVEN: Camera and IMU topics from the observed Alphasense bag layout
        camera_connections = [
            SimpleNamespace(topic="/alphasense_driver_ros/cam0/compressed", msgtype="sensor_msgs/msg/CompressedImage"),
            SimpleNamespace(topic="/camera/rear/image/compressed", msgtype="sensor_msgs/CompressedImage"),
        ]
        imu_connections = [
            SimpleNamespace(topic="/alphasense_driver_ros/imu", msgtype="sensor_msgs/msg/Imu"),
            SimpleNamespace(topic="/rear_imu/data", msgtype="sensor_msgs/Imu"),
        ]

        # WHEN: The topic mappings are inferred from the bag connections
        camera_topics = ros1_bag_to_mcap._infer_camera_topics(camera_connections)
        imu_topics = ros1_bag_to_mcap._infer_imu_topics(imu_connections)

        # THEN: The canonical MCAP topics match the stack's naming convention
        self.assertEqual(
            [topic.output_topic for topic in camera_topics],
            ["/camera/cam0/image/compressed", "/camera/rear/image/compressed"],
        )
        self.assertEqual([topic.accel_topic for topic in imu_topics], ["/imu/accelerometer", "/rear_imu/accelerometer"])
        self.assertEqual([topic.gyro_topic for topic in imu_topics], ["/imu/gyroscope", "/rear_imu/gyroscope"])

    def test_given_ros1_messages_when_conversion_runs_then_core_mcap_messages_are_written(self):
        # GIVEN: A fake bag reader with five camera streams and one IMU stream
        camera_connections = [
            SimpleNamespace(topic=f"/alphasense_driver_ros/cam{index}/compressed", msgtype="sensor_msgs/msg/CompressedImage")
            for index in range(5)
        ]
        imu_connection = SimpleNamespace(topic="/alphasense_driver_ros/imu", msgtype="sensor_msgs/msg/Imu")
        camera_messages = [
            SimpleNamespace(
                header=SimpleNamespace(stamp=SimpleNamespace(sec=1, nanosec=234_000_000 + index), frame_id=f"cam{index}"),
                format="jpeg",
                data=bytes([0x10 + index, 0x20 + index, 0x30 + index]),
            )
            for index in range(5)
        ]
        imu_message = SimpleNamespace(
            header=SimpleNamespace(stamp=SimpleNamespace(sec=4, nanosec=5), seq=42),
            linear_acceleration=SimpleNamespace(x=1.0, y=2.0, z=3.0),
            angular_velocity=SimpleNamespace(x=4.0, y=5.0, z=6.0),
        )
        fake_reader = _FakeReader(
            [(camera_connections[index], 0, camera_messages[index]) for index in range(5)]
            + [(imu_connection, 0, imu_message)]
        )
        fake_writer = _FakeWriter()
        fake_handle = _FakeHandle()

        # WHEN: The converter runs against the fake adapter layer
        with mock.patch.object(ros1_bag_to_mcap, "_open_rosbag_reader", return_value=fake_reader), mock.patch.object(
            ros1_bag_to_mcap, "_open_mcap_writer", return_value=(fake_writer, fake_handle)
        ):
            ros1_bag_to_mcap.convert_ros1_bag_to_mcap(
                "input.bag",
                "output.mcap",
            )

        # THEN: The MCAP writer receives the expected schemas, channels, and translated messages
        self.assertTrue(fake_writer.finished)
        self.assertTrue(fake_handle.closed)
        self.assertEqual([schema[1] for schema in fake_writer.schemas], [ros1_bag_to_mcap.COMPRESSED_IMAGE_SCHEMA_NAME, ros1_bag_to_mcap.IMU_SCHEMA_NAME])
        self.assertEqual(
            [channel[1] for channel in fake_writer.channels],
            [
                "/camera/cam0/image/compressed",
                "/camera/cam1/image/compressed",
                "/camera/cam2/image/compressed",
                "/camera/cam3/image/compressed",
                "/camera/cam4/image/compressed",
                "/imu/accelerometer",
                "/imu/gyroscope",
            ],
        )
        self.assertEqual(len(fake_writer.messages), 7)

        for index in range(5):
            channel_id, log_time, camera_data, publish_time, sequence = fake_writer.messages[index]
            self.assertEqual(channel_id, f"channel-{index + 1}")
            self.assertEqual(log_time, 1_234_000_000 + index)
            self.assertEqual(publish_time, 1_234_000_000 + index)
            self.assertEqual(sequence, 0)
            self.assertEqual(json.loads(camera_data.decode("utf-8"))["frame_id"], f"cam{index}")

        accel_channel_id, accel_log_time, accel_data, accel_publish_time, accel_sequence = fake_writer.messages[5]
        gyro_channel_id, gyro_log_time, gyro_data, gyro_publish_time, gyro_sequence = fake_writer.messages[6]
        self.assertEqual(accel_channel_id, "channel-6")
        self.assertEqual(gyro_channel_id, "channel-7")
        self.assertEqual(accel_log_time, 4_000_000_005)
        self.assertEqual(gyro_log_time, 4_000_000_005)
        self.assertEqual(accel_publish_time, 4_000_000_005)
        self.assertEqual(gyro_publish_time, 4_000_000_005)
        self.assertEqual(accel_sequence, 42)
        self.assertEqual(gyro_sequence, 42)

        accel_json = json.loads(accel_data.decode("utf-8"))
        gyro_json = json.loads(gyro_data.decode("utf-8"))
        self.assertEqual(accel_json["x"], 1.0)
        self.assertEqual(accel_json["y"], 2.0)
        self.assertEqual(accel_json["z"], 3.0)
        self.assertEqual(gyro_json["x"], 4.0)
        self.assertEqual(gyro_json["y"], 5.0)
        self.assertEqual(gyro_json["z"], 6.0)

    def test_given_camera_topic_with_wrong_message_type_when_conversion_runs_then_it_fails_fast(self):
        # GIVEN: A camera-shaped topic with the wrong ROS message type
        camera_connection = SimpleNamespace(topic="/camera/front/image/compressed", msgtype="sensor_msgs/Image")
        camera_message = SimpleNamespace(
            header=SimpleNamespace(stamp=SimpleNamespace(sec=1, nanosec=0), frame_id="front"),
            format="jpeg",
            data=b"\x10\x20\x30",
        )
        fake_reader = _FakeReader([(camera_connection, 0, camera_message)])
        fake_writer = _FakeWriter()
        fake_handle = _FakeHandle()

        # WHEN: The converter runs
        with mock.patch.object(ros1_bag_to_mcap, "_open_rosbag_reader", return_value=fake_reader), mock.patch.object(
            ros1_bag_to_mcap, "_open_mcap_writer", return_value=(fake_writer, fake_handle)
        ):
            # THEN: A clear validation error is raised
            with self.assertRaisesRegex(ValueError, "sensor_msgs/CompressedImage"):
                ros1_bag_to_mcap.convert_ros1_bag_to_mcap(
                    "input.bag",
                    "output.mcap",
                )

    def test_given_duplicate_inferred_camera_outputs_when_conversion_starts_then_it_fails_fast(self):
        # GIVEN: Two camera topics that would map to the same inferred MCAP topic
        first_connection = SimpleNamespace(topic="/front_left/image/compressed", msgtype="sensor_msgs/CompressedImage")
        second_connection = SimpleNamespace(topic="/camera/front_left/image/compressed", msgtype="sensor_msgs/CompressedImage")
        fake_reader = _FakeReader([(first_connection, 0, SimpleNamespace()), (second_connection, 0, SimpleNamespace())])
        fake_writer = _FakeWriter()
        fake_handle = _FakeHandle()

        # WHEN: The converter inspects the bag connections
        with mock.patch.object(ros1_bag_to_mcap, "_open_rosbag_reader", return_value=fake_reader), mock.patch.object(
            ros1_bag_to_mcap, "_open_mcap_writer", return_value=(fake_writer, fake_handle)
        ):
            # THEN: The conflicting inferred topic is rejected
            with self.assertRaisesRegex(ValueError, "same output topic"):
                ros1_bag_to_mcap.convert_ros1_bag_to_mcap("input.bag", "output.mcap")


if __name__ == "__main__":
    unittest.main()
