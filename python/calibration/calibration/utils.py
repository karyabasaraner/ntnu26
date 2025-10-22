import yaml
import matplotlib.pyplot as plt
from pathlib import Path


def read_config(config_file: str):
    config_file = Path(config_file)
    with config_file.open("r") as file:
        config = yaml.safe_load(file)
    return config

def plot_camera_tags_gt(tags_dict, pose_inertial_rig, camera_poses_rig_cam):
    fig = plt.figure()
    ax = fig.add_subplot(111, projection='3d')

    # Plot tags
    for tag_id, tag in tags_dict.items():
        corners = tag.corners
        ax.scatter(corners[0, :], corners[1, :], corners[2, :], label=f"Tag {tag_id}")
        for i in range(4):
            ax.text(corners[0, i], corners[1, i], corners[2, i], f"{i}")

        ax.text(tag.center_pos[0], tag.center_pos[1], tag.center_pos[2], f"Tag {tag_id}")

    plot_camera_pose(ax, pose_inertial_rig, "rig", ("k", "k", "y"))

    for i, pose_rig_cam in enumerate(camera_poses_rig_cam):
        pose_inertial_cam = pose_inertial_rig @ pose_rig_cam
        plot_camera_pose(ax, pose_inertial_cam, f"cam{i}", ("r", "g", "b"))

    ax.set_xlabel('X')
    ax.set_ylabel('Y')
    ax.set_zlabel('Z')
    ax.set_box_aspect((1, 1, 1))
    ax.view_init(elev=30, azim=130, roll=0)

    plt.tight_layout()
    plt.show()
    plt.close()

def plot_camera_pose(ax, pose, name, colors):
    # Add a three arrow to represent x, y, z axis
    x, y, z = pose[0:3, 3]
    x0, x1, x2 = pose[0:3, 0]
    y0, y1, y2 = pose[0:3, 1]
    z0, z1, z2 = pose[0:3, 2]
    ax.quiver(x, y, z, x0, x1, x2, length=0.2, color=colors[0])
    ax.quiver(x, y, z, y0, y1, y2, length=0.2, color=colors[1])
    ax.quiver(x, y, z, z0, z1, z2, length=0.2, color=colors[2])
    ax.text(x, y, z, name, size=8)

def plot_3d_corners(corners_3d, ids):
    fig = plt.figure()
    ax = fig.add_subplot(111, projection='3d')

    for corner, id in zip(corners_3d, ids):
        ax.scatter(corner[:, 0], corner[:, 1], corner[:, 2], c='b', s=3)
        ax.text(corner[0, 0], corner[0, 1], corner[0, 2], f"Tag {id}")
    ax.set_xlabel('X')
    ax.set_ylabel('Y')
    ax.set_zlabel('Z')
    ax.set_box_aspect((1, 1, 1))
    plt.tight_layout()
    plt.show()
