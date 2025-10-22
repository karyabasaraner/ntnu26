import cv2
import logging
import numpy as np

logging.basicConfig(level=logging.INFO)
logger = logging.getLogger(__name__)

class SingleViewCalib:
    """
    Calibrate intrinsics and extrinsics from a single view of AprilTags with known 3D positions.
    """
    def __init__(self, tags_dict, observations):
        ret, K, D, R, t = self._calibrate(tags_dict, observations)
        self.reprojection_error = ret
        self.K = K
        self.dist_coeffs = D
        self.T_camera_world = np.eye(4)
        if R is not None and t is not None:
            self.T_camera_world[0:3, 0:3] = R
            self.T_camera_world[0:3, 3] = t.flatten()

    def _is_degenerate(self, object_points):
        n_points = len(object_points)

        # 1. Check minimum number of correspondences
        MIN_TAGS = 2  # For fisheye, need at least 8 points (2 tags minimum)
        if n_points < MIN_TAGS:
            logger.warning(f"Too few correspondences: {n_points} < {MIN_TAGS}")
            return True

        # 2. Check if points are coplanar (all in same plane), compute PCA on 3D points
        object_points_combined = np.concatenate(object_points, axis=0)
        mean = np.mean(object_points_combined, axis=0)
        centered = object_points_combined - mean

        # Singular values tell us variance along principal axes
        _, s, _ = np.linalg.svd(centered)

        # If smallest singular value is very small, points are nearly coplanar
        planarity_ratio = s[2] / s[0]  # ratio of smallest to largest
        PLANARITY_THRESHOLD = 0.01

        if planarity_ratio < PLANARITY_THRESHOLD:
            logger.warning(f"Points are nearly coplanar: ratio={planarity_ratio:.6f}")
            return True
        return False

    def _calibrate(self, tags_dict, observations):
        object_points = []
        image_points = []

        for obs in observations:
            shape = (obs.width, obs.height)
            tag = tags_dict.get(obs.id, None)
            if tag is None:
                continue

            corners3d = tag.corners[:, 0:3]  # 4x3
            corners2d = obs.corners          # 4x2

            object_points.append(corners3d)
            image_points.append(corners2d)

        if self._is_degenerate(object_points):
            return None, None, None, None, None

        # Combine all points and convert to proper format
        # Fisheye requires shape (1, N, 3) for object points and (1, N, 2) for image points
        object_points_combined = np.concatenate(object_points, axis=0)
        image_points_combined = np.concatenate(image_points, axis=0)

        object_points_list = [object_points_combined.reshape(1, -1, 3)]
        image_points_list = [image_points_combined.reshape(1, -1, 2)]

        # Fisheye calibration
        calibration_flags = (
            cv2.fisheye.CALIB_FIX_SKEW +
            cv2.fisheye.CALIB_CHECK_COND +
            cv2.fisheye.CALIB_RECOMPUTE_EXTRINSIC
        )

        K = np.zeros((3, 3))
        D = np.zeros((4, 1))  # Fisheye distortion: 4 coefficients (k1, k2, k3, k4)
        rvecs = [np.zeros((1, 1, 3), dtype=np.float32)]
        tvecs = [np.zeros((1, 1, 3), dtype=np.float32)]

        try:
            ret, K, D, rvecs, tvecs = cv2.fisheye.calibrate(
                object_points_list,
                image_points_list,
                shape,
                K,
                D,
                rvecs,
                tvecs,
                calibration_flags
            )

            t = tvecs[0]
            R, _ = cv2.Rodrigues(rvecs[0])
            logger.info(f"Calibration results: {ret:.3f}")
        except Exception as e:
            logger.error(f"Calibration failed: {e}")
            return None, None, None, None, None

        return ret, K, D, R, t
