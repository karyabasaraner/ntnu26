import pickle
import logging
from calibration import *
from pathlib import Path

logging.basicConfig(level=logging.DEBUG)


def main():
    config = read_config("python/calibration/config/config.yaml")
    dataloader = Dataloader(config)
    data_root = Path(config["data_root"])

    # NOTE: This needs to be replaced with an actual initial estimate
    camera_gt_dict = read_camera_gt_data(data_root / "camera_data.txt")

    tags_inertial_dict = read_tag_data(data_root / "tag_data.txt")

    cam_dict = {}
    cam_dict_pkl_file = data_root / "camera_dict.pkl"
    if not cam_dict_pkl_file.exists():
        for cam_name, index, image in dataloader:
            if cam_name not in cam_dict:
                # NOTE: This needs to be replaced / removed.
                pose_rig_cam = camera_gt_dict[cam_name]["pose_rig_cam"]
                K = camera_gt_dict[cam_name]["K"]
                d = camera_gt_dict[cam_name]["d"]
                cam_dict[cam_name] = Camera(cam_name, K, d, pose_rig_cam)

            # NOTE: This needs to be replaced / removed.
            pose_inertial_rig = camera_gt_dict["cam0"]["poses_inertial_rig"][index]
            cam_dict[cam_name].add_view(index, image, pose_inertial_rig, tags_inertial_dict)

        with cam_dict_pkl_file.open("wb") as file:
            pickle.dump(cam_dict, file)

    else:
        with cam_dict_pkl_file.open("rb") as file:
            cam_dict = pickle.load(file)

    ro = RigOptimization(tags_inertial_dict, cam_dict, config)

    # # Test residual computation
    # camera_poses_rig_cam = []
    # for cam_name, cam_data in camera_gt_dict.items():
    #     camera_poses_rig_cam.append(cam_data["pose_rig_cam"])

    # for cam_name, camera in cam_dict.items():
    #     print(camera)
    #     for view in camera.views:
    #         frame = view.index
    #         # plot_camera_tags_gt(tags_inertial_dict, view.pose_inertial_rig, camera_poses_rig_cam)
    #         residuals = view.compute_residual(camera.K, camera.d, camera.pose_rig_cam)
    #         if residuals.size:
    #             print(f"View {frame}, mean residuals: {np.mean(np.linalg.norm(residuals, axis=1)):.2f} px")


if __name__ == "__main__":
    main()