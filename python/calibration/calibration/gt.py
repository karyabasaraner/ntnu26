import numpy as np
from scipy.spatial.transform import Rotation
from pathlib import Path

def read_camera_gt_data(camera_file: Path):
    with camera_file.open("r") as file:
        lines = file.readlines()

    lines = lines[1:]  # Remove header

    # camera_name, frame, time_sec, x, y, z, qx, qy, qz, qw, fx, fy, cx, cy
    camera_dict = {}
    for line in lines:
        parts = line.strip().split(sep=", ")
        camera_name = parts[0]
        frame = int(parts[1])
        x, y, z = map(float, parts[3:6])
        qx, qy, qz, qw = map(float, parts[6:10])

        R_blender = Rotation.from_quat([qx, qy, qz, qw], scalar_first=False).as_matrix().T
        R_normal_blender = Rotation.from_euler('x', 180, degrees=True).as_matrix()
        R = R_blender
        t = np.array([x, y, z]).reshape(3, 1)
        T = np.eye(4)
        T[0:3, 0:3] = R
        T[0:3, 3] = t.flatten()

        # Add a new pose at a given time
        if camera_name not in camera_dict:
            fx, fy, cx, cy = map(float, parts[10:14])
            K = np.eye(3)
            K[0, 0] = fx
            K[1, 1] = fy
            K[0, 2] = cx
            K[1, 2] = cy
            d = np.zeros(4)  # Assume no distortion
            camera_dict[camera_name] = {"K": K, "d": d, "poses_inertial_rig": {}}

        # Pose of the camera in inertial frame
        camera_dict[camera_name]["poses_inertial_rig"][frame] = T

    for cam_name, cam_data in camera_dict.items():
        pose_inertial_rig = camera_dict["cam0"]["poses_inertial_rig"][1]
        pose_inertial_cam = camera_dict[cam_name]["poses_inertial_rig"][1]
        pose_rig_cam = np.linalg.inv(pose_inertial_rig) @ pose_inertial_cam
        camera_dict[cam_name]["pose_rig_cam"] = pose_rig_cam

    return camera_dict
