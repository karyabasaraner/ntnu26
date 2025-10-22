import numpy as np

from scipy.spatial.transform import Rotation
from pathlib import Path

class Tag:
    def __init__(self, id, size, center_pos, center_q):
        self.id = id
        self.size = size
        self.center_pos = center_pos
        self.center_q = center_q
        self.T_tw = np.eye(4)
        self.T_tw[0:3, 0:3] = Rotation.from_quat(center_q, scalar_first=False).as_matrix()
        self.T_tw[0:3, 3] = np.array(center_pos)
        self.corners = self._get_tag_corners()

    def __str__(self):
        return f"Tag(id={self.id}, size={self.size}, center_pos={self.center_pos}, R_tw={self.R_tw})"

    def _get_tag_corners(self):
        half_size = self.size / 2.0
        corners = [
            np.array([-half_size,  half_size, 0, 1]),
            np.array([ half_size,  half_size, 0, 1]),
            np.array([ half_size, -half_size, 0, 1]),
            np.array([-half_size, -half_size, 0, 1]),
        ]
        corners_world = []
        for corner in corners:
            corners_world.append(self.T_tw @ corner)
        return np.array(corners_world).T


def read_tag_data(tag_file: Path):
    with tag_file.open("r") as file:
        lines = file.readlines()

    lines = lines[1:]  # Remove header

    # tag_id, x, y, z, qx, qy, qz, qw, size
    tags_dict = {}
    for line in lines:
        parts = line.strip().split(sep=", ")
        tag_id = int(parts[0])
        x, y, z = map(float, parts[1:4])
        qx, qy, qz, qw = map(float, parts[4:8])
        size = float(parts[8])
        tag = Tag(tag_id, size, (x, y, z), (qx, qy, qz, qw))
        tags_dict[tag_id] = tag

    return tags_dict
