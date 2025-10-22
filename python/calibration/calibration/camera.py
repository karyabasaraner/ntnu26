import apriltag
import cv2
import numpy as np
import matplotlib.pyplot as plt

from pathlib import Path
from scipy.spatial.transform import Rotation

from .utils import plot_3d_corners

class Camera:
    """
    Camera class that tracks intrinsics, extrinsics, and all views (over time).
    """
    def __init__(self, name, K, d, pose_rig_cam):
        self.name = name
        self.K = K
        self.d = d
        self.pose_world_dict = {}  # key: frame/time_sec, value: 4x4 pose matrix
        self.pose_rig_cam = pose_rig_cam
        self.views = []

    def __str__(self):
        n_tags = 0
        for view in self.views:
            n_tags += len(view.tags)
        return f"{self.name}: fx:{self.K[0, 0]}, fy:{self.K[1, 1]}, views={len(self.views)}, tags={n_tags}"

    def set_pose_rig(self, T):
        self.pose_rig = T

    def add_view(self, index, image, pose_inertial_rig, tags_inertial_dict):
        self.views.append(View(index, image, pose_inertial_rig, tags_inertial_dict))


class View:
    """
    View class that stores all tag observations for a single view / frame.
    """
    def __init__(self, index, image, pose_inertial_rig, tags_inertial_dict):
        self.tags = []
        self.index = index
        self.pose_inertial_rig = pose_inertial_rig
        self.detect_tags(image, tags_inertial_dict)
        # self.image = cv2.cvtColor(image, cv2.COLOR_BGR2RGB)
        # TODO: Attempt to restore an initial pose estimate from PnP here?

    def detect_tags(self, image, tags_inertial_dict):
        detector = apriltag.Detector(apriltag.DetectorOptions(families='tag36h11', nthreads=16, quad_blur=1.0))
        image_gray = cv2.cvtColor(image, cv2.COLOR_BGR2GRAY)
        tags = detector.detect(image_gray)

        for tag in tags:
            tag_id = tag.tag_id
            corners = tag.corners  # corners is a 4x2 array of pixel coordinates
            tag_inertial = tags_inertial_dict.get(tag_id, None)
            if tag_inertial is not None:
                corners_inertial = tag_inertial.corners
            else:
                corners_inertial = None

            self.tags.append(TagObservation(tag_id, corners, corners_inertial))

    def add_tag(self, tag):
        self.tags.append(tag)

    def compute_residual(self, K, d, pose_rig_cam):
        residuals = []
        corner_view_all = []
        for tag in self.tags:
            pose_rig_inertial = np.linalg.inv(self.pose_inertial_rig)

            # Transform from inertial to camera frame
            pose_cam_inertial = np.linalg.inv(pose_rig_cam) @ pose_rig_inertial

            # Corners in camera frame
            corners_cam = pose_cam_inertial @ tag.corners_3d

            corner_view_all.append(corners_cam[:3, :].T)
            corners_image = self.fisheye_project_points(K, d, corners_cam)
            residual = corners_image - tag.corners_2d # TODO: SIGN?
            residuals.append(residual)
            # self.plot_view_residuals(self.image, tag.corners_2d, corners_image, residual)

        # plot_3d_corners(corner_view_all, [tag.id for tag in self.tags])
        return np.array(residuals)

    def plot_view_residuals(self, image, corners_2d, corners_image, residual):
        fig, ax = plt.subplots()
        ax.imshow(image)
        ax.scatter(corners_2d[:, 0], corners_2d[:, 1], c='b', label='Observed', s=4)
        ax.scatter(corners_image[:, 0], corners_image[:, 1], c='y', label='Projected', s=4)
        plt.title(f"Mean residual: {np.mean(np.linalg.norm(residual, axis=1)):.2f} px")
        plt.legend()
        plt.show()

    def fisheye_project_points(self, K, d, points_camera):
        # https://docs.opencv.org/4.12.0/db/d58/group__calib3d__fisheye.html
        x, y, z = points_camera[0, :], points_camera[1, :], points_camera[2, :]
        a = x / z
        b = y / z

        r2 = a**2 + b**2
        r = np.sqrt(r2)
        r = np.where(r < 1e-8, 1e-8, r)
        theta = np.arctan(r)

        theta_d = theta * (1 + d[0]*theta**2 + d[1]*theta**4 + d[2]*theta**6 + d[3]*theta**8)
        scale = theta_d / r

        x_distorted = a * scale
        y_distorted = b * scale

        u = K[0, 0] * x_distorted + K[0, 2]
        v = K[1, 1] * y_distorted + K[1, 2]
        return np.vstack((u, v)).T
    
    def pinhole_project_points(self, K, d, points_camera):
        # Simple pinhole projection without distortion
        x, y, z = points_camera[0, :], points_camera[1, :], points_camera[2, :]
        u = K[0, 0] * (x / z) + K[0, 2]
        v = K[1, 1] * (y / z) + K[1, 2]
        return np.vstack((u, v)).T

class TagObservation:
    def __init__(self, id, corners_2d, corners_3d):
        self.id = id
        self.corners_2d = corners_2d
        self.corners_3d = corners_3d

    def __str__(self):
        return f"Tag: {self.id}"
