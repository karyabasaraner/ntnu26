import logging
import numpy as np
from concurrent.futures import ThreadPoolExecutor

from pathlib import Path
from scipy.optimize import least_squares
from scipy.spatial.transform import Rotation
import yaml

logger = logging.getLogger(__name__)

class RigOptimization:
    def __init__(self, tags_inertial_dict, cam_dict, config):
        self.ref_camera = config["ref_camera"]
        self.data_root = Path(config["data_root"])
        self.tags_inertial_dict = tags_inertial_dict
        self.cam_dict = cam_dict
        self.timestamps = self._get_timestamps()
        self.camera_names = list(cam_dict.keys())
        self._ls_iter = 0
        self._last_residuals = None

        # Parallel settings
        self.parallel = config.get("parallel", True)
        self.max_workers = config.get("max_workers", None)
        self._executor = ThreadPoolExecutor(max_workers=self.max_workers) if self.parallel else None

        # Run optimization and apply results back into cam_dict
        results = self._optimize()
        if results.success:
            intrinsics, distortion, rig_extrinsics, rig_poses = self._unpack_parameters(results.x)
            self._apply_parameters_to_cam_dict(intrinsics, distortion, rig_extrinsics, rig_poses)
            self._write_camera_yaml(intrinsics, distortion, rig_extrinsics, filename=self.data_root / "calibration.yaml")

    def _get_timestamps(self):
        ref_cam = self.cam_dict.get(self.ref_camera, None)
        if ref_cam is not None:
            timestamps = [view.index for view in ref_cam.views]
            timestamps = sorted(list(set(timestamps)))
            return timestamps
        return None

    def _optimize(self):
        logger.info("Starting rig optimization...")
        x0 = self._pack_parameters()

        result = least_squares(
            self._residuals,
            x0,
            method='trf',
            loss='huber',
            callback=self._ls_callback,
            verbose=2,
            ftol=1e-6,
            xtol=1e-6,
            max_nfev=200
        )

        return result

    def _ls_callback(self, xk, *args, **kwargs):
        self._ls_iter += 1
        try:
            res = self._residuals(xk)
            cost = 0.5 * np.sum(res**2)

            # compute mean reprojection error (per observation) in pixels
            if res.size == 0:
                mean_err = 0.0
            else:
                if res.size % 2 == 0:
                    pts = res.reshape(-1, 2)
                    mean_err = float(np.mean(np.linalg.norm(pts, axis=1)))
                else:
                    # fallback if residuals are 1D
                    mean_err = float(np.mean(np.abs(res)))

            msg = (f"[least_squares] iter={self._ls_iter}, cost={cost:.6e}, "
                   f"||res||={np.linalg.norm(res):.6e}, mean_reproj_px={mean_err:.4f}")
            logger.info(msg)
            print(msg, flush=True)
        except Exception as e:
            logger.debug(f"_ls_callback: failed to compute diagnostics: {e}")

    def compute_view_residual(self, cam_name, view, intrinsics, distortion, rig_extrinsics, rig_poses, ref_camera):
        K = intrinsics[cam_name]
        d = distortion[cam_name]

        # TODO: Is this clean?
        T_cam_rig = rig_extrinsics.get(cam_name, np.eye(4))

        timestamp = view.index
        if timestamp not in rig_poses:
            return None

        T_rig_world = rig_poses[timestamp]
        orig_pose = view.pose_inertial_rig
        view.pose_inertial_rig = T_rig_world
        res = view.compute_residual(K, d, T_cam_rig)
        view.pose_inertial_rig = orig_pose

        if res.size:
            return res.flatten()
        return None

    def _residuals(self, params):
        intrinsics, distortion, rig_extrinsics, rig_poses = self._unpack_parameters(params)
        jobs = []
        for cam_name in self.camera_names:
            cam = self.cam_dict[cam_name]
            for view in cam.views:
                jobs.append((cam_name, view, intrinsics, distortion, rig_extrinsics, rig_poses, self.ref_camera))

        if self._executor is not None and len(jobs) > 0:
            # Use a thread pool to avoid pickling and process spin-up per evaluation
            results = list(self._executor.map(lambda args: self.compute_view_residual(*args), jobs))
        else:
            # Fallback to sequential
            results = [self.compute_view_residual(*args) for args in jobs]

        # Flatten and filter out None
        residuals = np.array([item for res in results if res is not None for item in res])
        self._last_residuals = residuals
        return residuals

    def __del__(self):
        try:
            if hasattr(self, "_executor") and self._executor is not None:
                self._executor.shutdown(wait=False)
        except Exception:
            pass

    def _pack_parameters(self):
        params = []
        for cam_name in self.camera_names:
            cam = self.cam_dict[cam_name]
            params.extend([float(cam.K[0, 0]), float(cam.K[1, 1]), float(cam.K[0, 2]), float(cam.K[1, 2])])

        for cam_name in self.camera_names:
            cam = self.cam_dict[cam_name]
            params.extend([float(cam.d[0]), float(cam.d[1]), float(cam.d[2]), float(cam.d[3])])

        for cam_name in self.camera_names:
            if cam_name != self.ref_camera:
                cam = self.cam_dict[cam_name]
                T = cam.pose_rig_cam
                rvec = Rotation.from_matrix(T[:3, :3]).as_rotvec()
                tvec = T[:3, 3]
                params.extend(rvec.tolist())
                params.extend(tvec.tolist())

        ref_cam = self.cam_dict[self.ref_camera]
        ref_pose_map = {view.index: view.pose_inertial_rig for view in ref_cam.views}
        for timestamp in self.timestamps:
            T = ref_pose_map[timestamp]
            rvec = Rotation.from_matrix(T[:3, :3]).as_rotvec()
            tvec = T[:3, 3]
            params.extend(rvec.tolist())
            params.extend(tvec.tolist())

        return np.array(params)

    def _unpack_parameters(self, params):
        idx = 0

        intrinsics = {}
        for cam_name in self.camera_names:
            K = np.eye(3)
            K[0, 0] = params[idx]
            K[1, 1] = params[idx + 1]
            K[0, 2] = params[idx + 2]
            K[1, 2] = params[idx + 3]
            intrinsics[cam_name] = K
            idx += 4

        distortion = {}
        for cam_name in self.camera_names:
            d = params[idx:idx + 4]
            distortion[cam_name] = d
            idx += 4

        rig_extrinsics = {}
        for cam_name in self.camera_names:
            if cam_name != self.ref_camera:
                rvec = params[idx:idx + 3]
                tvec = params[idx + 3:idx + 6]
                R = Rotation.from_rotvec(rvec).as_matrix()
                T = np.eye(4)
                T[:3, :3] = R
                T[:3, 3] = tvec
                rig_extrinsics[cam_name] = T
                idx += 6

        rig_poses = {}
        for timestamp in self.timestamps:
            rvec = params[idx:idx + 3]
            tvec = params[idx + 3:idx + 6]
            T = np.eye(4)
            T[:3, :3] = Rotation.from_rotvec(rvec).as_matrix()
            T[:3, 3] = tvec
            rig_poses[timestamp] = T
            idx += 6
        return intrinsics, distortion, rig_extrinsics, rig_poses

    def _apply_parameters_to_cam_dict(self, intrinsics, distortion, rig_extrinsics, rig_poses):
        """Write optimized parameters back into the Camera and View objects in cam_dict."""
        # Update intrinsics and distortion
        for cam_name in self.camera_names:
            cam = self.cam_dict.get(cam_name, None)
            if cam is None:
                continue
            cam.K = intrinsics[cam_name]
            cam.d = distortion[cam_name]

            # Update rig extrinsic for non-ref cameras
            if cam_name == self.ref_camera:
                cam.pose_rig_cam = np.eye(4)
            else:
                cam.pose_rig_cam = rig_extrinsics.get(cam_name, cam.pose_rig_cam)

        # Update views' rig poses (pose_inertial_rig) so that subsequent residual
        # computations use the optimized rig trajectory
        for cam_name in self.camera_names:
            cam = self.cam_dict.get(cam_name, None)
            if cam is None:
                continue
            for view in cam.views:
                if view.index in rig_poses:
                    view.pose_inertial_rig = rig_poses[view.index]

    def _write_camera_yaml(self, intrinsics, distortion, rig_extrinsics, filename=None):
        cameras_out = []
        for cam_name in self.camera_names:
            cam = self.cam_dict.get(cam_name, None)
            if cam is None:
                continue

            K = intrinsics.get(cam_name, None)
            d = distortion.get(cam_name, None)

            # intrinsics in order [fx, fy, cx, cy]
            if K is not None:
                intr_list = [float(K[0, 0]), float(K[1, 1]), float(K[0, 2]), float(K[1, 2])]
            else:
                intr_list = None

            dist_list = None
            if d is not None:
                dist_list = [float(x) for x in np.asarray(d).tolist()]

            # resolution: try to get from first view image
            resolution = None
            if hasattr(cam, 'views') and len(cam.views) > 0:
                view = cam.views[0]
                if hasattr(view, 'image') and view.image is not None:
                    h, w = view.image.shape[:2]
                    resolution = [int(w), int(h)]

            # extrinsics: use rig_extrinsics entry if available, otherwise cam.pose_rig_cam
            T = None
            if cam_name == self.ref_camera:
                T = np.eye(4)
            else:
                T = rig_extrinsics.get(cam_name, None)
                if T is None:
                    T = getattr(cam, 'pose_rig_cam', None)

            pose_list = None
            if T is not None:
                pose_list = np.asarray(T, dtype=float).tolist()

            cam_entry = {
                'name': cam_name,
                'camera_model': 'pinhole',
                'distortion_model': 'equidistant',
                'distortion_coeffs': dist_list if dist_list is not None else [0.0, 0.0, 0.0, 0.0],
                'intrinsics': intr_list if intr_list is not None else [],
            }

            if resolution is not None:
                cam_entry['resolution'] = resolution

            if pose_list is not None:
                cam_entry['pose_rig_cam'] = pose_list

            cameras_out.append(cam_entry)

        out = {'cameras': cameras_out}

        # Write YAML
        with open(filename, 'w') as f:
            yaml.safe_dump(out, f, sort_keys=False)
