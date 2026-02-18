#include "camera.hpp"

#include "../utils/configs.hpp"

#include <cstddef>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <iostream>
#include <linux/videodev2.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/poll.h>
#include <unistd.h>
#include <unordered_map>
#include <utility>

namespace core {

// NOLINTNEXTLINE(clang-diagnostic-global-constructors)
const std::unordered_map<std::string, uint32_t> Camera::FOURCC_FORMATS = {
    {"RGB24", V4L2_PIX_FMT_RGB24}
};

Camera::Camera(CameraConfig config) : _config(std::move(config)) {
    _open_device();

    if (!is_valid()) {
        return;
    }

    _configure();

    if (!_init_mmap()) {
        return;
    }
}

Camera::~Camera() {
    stop();
    if (is_valid()) {
        for (const auto buffer : _buffers) {
            munmap(buffer.start, buffer.length);
        }
        close(_file_desc);
    }
}

bool Camera::start() {
    v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg,hicpp-vararg)
    if (ioctl(_file_desc, VIDIOC_STREAMON, &type) < 0) {
        std::cout << "Start streaming " << _config.device << ", " << errno << '\n';
        return false;
    }

    _running = true;
    _worker = std::thread(&Camera::_capture_loop, this);
    return true;
}

void Camera::stop() {
    _running = false;
    if (_worker.joinable()) {
        _worker.join();
    }

    v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg,hicpp-vararg)
    if (ioctl(_file_desc, VIDIOC_STREAMOFF, &type) < 0) {
        std::cout << "Stop streaming for device: " << _config.device << ", " << errno << '\n';
    }
}


bool Camera::_open_device() {
    if (!std::filesystem::exists(_config.device)) {
        std::cout << "Camera device does not exist: " << _config.device << '\n';
        return false;
    }

    _file_desc = open(_config.device.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC, 0);
    if (_file_desc < 0) {
        std::cout << "Open camera device: " << _config.device << ", " << errno << '\n';
        return false;
    }
    return true;
}

bool Camera::_configure() const {
    struct v4l2_format fmt{};
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

    // NOLINTBEGIN(cppcoreguidelines-pro-type-union-access)
    fmt.fmt.pix.width = _config.width;
    fmt.fmt.pix.height = _config.height;
    fmt.fmt.pix.pixelformat = FOURCC_FORMATS.at(_config.format);
    fmt.fmt.pix.field = V4L2_FIELD_NONE;
    // NOLINTEND(cppcoreguidelines-pro-type-union-access)

    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg,hicpp-vararg)
    if (ioctl(_file_desc, VIDIOC_S_FMT, &fmt) < 0) {
        std::cerr << "Set format: " << _config.device << ", " << errno << "\n";
        return false;
    }
    return true;
}

bool Camera::_init_mmap() {
    struct v4l2_requestbuffers req{};
    req.count = REQ_BUFFER_COUNT;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;

    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg,hicpp-vararg)
    if (ioctl(_file_desc, VIDIOC_REQBUFS, &req) < 0) {
        std::cerr << "Request buffers " << _config.device << ", " << errno << "\n";
        return false;
    }

    _buffers.resize(req.count);
    for (size_t index = 0; index < REQ_BUFFER_COUNT; ++index) {
        struct v4l2_buffer buf{};
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = index;

        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg,hicpp-vararg)
        if (ioctl(_file_desc, VIDIOC_QUERYBUF, &buf) < 0) {
            std::cerr << "Query buffer " << _config.device << ", " << errno << "\n";
            return false;
        }

        _buffers[index].length = buf.length;
        _buffers[index].start = mmap(
            nullptr,
            buf.length,
            PROT_READ | PROT_WRITE,
            MAP_SHARED,
            _file_desc,
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-union-access)
            buf.m.offset
        );

        // Queue buffer for capture
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg,hicpp-vararg)
        if (ioctl(_file_desc, VIDIOC_QBUF, &buf) < 0) {
            std::cerr << "Queue buffer " << _config.device << ", " << errno << "\n";
            return false;
        }
    }

    return true;
}

void Camera::_capture_loop() {
    struct pollfd pfd{};
    pfd.fd = _file_desc;
    pfd.events = POLLIN;

    while (_running) {
        // TODO(MJ): Timeout?
        int const ret = poll(&pfd, 1, -1);
        if (ret < 0) {
            std::cout << "Poll error for device: " << _config.device << ", " << errno << '\n';
            break;
        }

        struct v4l2_buffer buf{};
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;

        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg,hicpp-vararg)
        if (ioctl(_file_desc, VIDIOC_DQBUF, &buf) == 0) {
            _process_frame(_buffers[buf.index].start, buf.bytesused);

            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg,hicpp-vararg)
            if (ioctl(_file_desc, VIDIOC_QBUF, &buf) < 0) {
                std::cout << "Re-queue buffer " << _config.device << ", " << errno << '\n';
                break;
            }
        } else {
            std::cout << "Dequeue buffer " << _config.device << ", " << errno << '\n';
            break;
        }
    }
}

void Camera::_process_frame(void* data, size_t length) const {
    std::cout << "Received frame of length: " << length << " from device: " << _config.device << '\n';
    data = data; // TODO(MJ): Process frame data here
}

} // namespace core
