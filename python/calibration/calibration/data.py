import cv2
from pathlib import Path

class Dataloader:
    def __init__(self, config):
        self.data_root = Path(config['data_root'])
        self.cameras_config = config['cameras']

    def __iter__(self):
        for camera_config in self.cameras_config:
            name = camera_config['name']
            cam_path = self.data_root / name
            image_files = sorted(cam_path.glob("*.jpg"))
            for image_file in image_files:
                timestamp = int(image_file.stem)
                image = cv2.imread(str(image_file))
                yield name, timestamp, image
