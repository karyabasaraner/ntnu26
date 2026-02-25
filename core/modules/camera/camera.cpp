#include "camera.hpp"

#include "../utils/configs.hpp"

#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <fcntl.h>
#include <filesystem>
#include <linux/videodev2.h>
#include <poll.h>
#include <spdlog/spdlog.h>
#include <string>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/poll.h>
#include <unistd.h>
#include <unordered_map>
#include <utility>

namespace core {

const std::unordered_map<std::string, uint32_t> Camera::FOURCC_FORMATS = {
    {"NV16", V4L2_PIX_FMT_NV16},
    {"UYVY", V4L2_PIX_FMT_UYVY}
};

Camera::Camera(CameraConfig config) : _config(std::move(config)), _shdict_writer(_config) {
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
    // NOTE: This closes the device and stops the capture thread if still running
    stop();
}

bool Camera::start() {
    v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg,hicpp-vararg)
    if (ioctl(_file_desc, VIDIOC_STREAMON, &type) < 0) {
        spdlog::warn("Failed to start streaming: {}, {}", _config.device, errno);
        return false;
    }

    _running = true;
    spdlog::info("Started streaming for camera: {}", _config.device);
    _worker = std::thread(&Camera::_capture_loop, this);
    return true;
}

void Camera::stop() {
    spdlog::info("Stopping camera: {}", _config.device);
    _running = false;
    if (_worker.joinable()) {
        _worker.join();
    }
    spdlog::info("Stopped capture thread for camera: {}", _config.device);

    v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg,hicpp-vararg)
    if (ioctl(_file_desc, VIDIOC_STREAMOFF, &type) < 0) {
        spdlog::warn("Failed to stop streaming for device: {}, {}", _config.device, errno);
    }
    spdlog::info("Stopped streaming for camera: {}", _config.device);

    if (is_valid()) {
        for (const auto buffer : _buffers) {
            munmap(buffer.start, buffer.length);
        }
        _close_device();
    }
}


bool Camera::_open_device() {
    if (is_valid()) {
        spdlog::info("Camera already open: {}", _config.device);
        return true;
    }

    if (!std::filesystem::exists(_config.device)) {
        spdlog::warn("Camera device does not exist: {}", _config.device);
        return false;
    }

    _file_desc = open(_config.device.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC, 0);
    if (_file_desc < 0) {
        spdlog::warn("Failed to open camera device: {}, {}", _config.device, errno);
        return false;
    }
    spdlog::info("Opened camera device: {}", _config.device);
    return true;
}

bool Camera::_close_device() {
    if (!is_valid()) {
        spdlog::info("Camera device already closed: {}", _config.device);
        return true;
    }

    if (close(_file_desc) < 0) {
        spdlog::warn("Failed to close camera device: {}, {}", _config.device, errno);
        return false;
    }
    _file_desc = -1;
    spdlog::info("Closed camera device: {}", _config.device);
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
        spdlog::warn("Failed to set format for device: {}, {}", _config.device, errno);
        return false;
    }
    spdlog::info("Configured camera device: {}", _config.device);
    return true;
}

bool Camera::_init_mmap() {
    struct v4l2_requestbuffers req{};
    req.count = _config.req_buffer_count;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;

    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg,hicpp-vararg)
    if (ioctl(_file_desc, VIDIOC_REQBUFS, &req) < 0) {
        spdlog::warn("Failed to request buffers for device: {}, {}", _config.device, errno);
        return false;
    }

    _buffers.resize(req.count);
    for (size_t index = 0; index < req.count; ++index) {
        struct v4l2_buffer buf{};
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = index;

        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg,hicpp-vararg)
        if (ioctl(_file_desc, VIDIOC_QUERYBUF, &buf) < 0) {
            spdlog::warn("Failed to query buffer for device: {}, {}", _config.device, errno);
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
            spdlog::warn("Failed to queue buffer for device: {}, {}", _config.device, errno);
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
        // NOTE: Block until a frame is available, most efficient
        int const ret = poll(&pfd, 1, -1);
        if (ret < 0) {
            spdlog::warn("Poll error for device: {}, {}", _config.device, errno);
            break;
        }

        struct v4l2_buffer buf{};
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;

        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg,hicpp-vararg)
        if (ioctl(_file_desc, VIDIOC_DQBUF, &buf) == 0) {
            const auto timestamp = std::chrono::steady_clock::now();
            _process_frame(_buffers[buf.index].start, buf.bytesused, timestamp);

            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg,hicpp-vararg)
            if (ioctl(_file_desc, VIDIOC_QBUF, &buf) < 0) {
                spdlog::warn("Failed to re-queue buffer for device: {}, {}", _config.device, errno);
                break;
            }
        } else {
            spdlog::warn("Failed to dequeue buffer for device: {}, {}", _config.device, errno);
            break;
        }
    }
}

void Camera::_process_frame(void* data, size_t length, const std::chrono::steady_clock::time_point& timestamp) {
    // NOTE: Copy as quickly as possible to free this thread for the next frame
    const uint64_t timestamp_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(timestamp.time_since_epoch()).count();
    _shdict_writer.add(_config.name, data, length, timestamp_ns);
}

} // namespace core
