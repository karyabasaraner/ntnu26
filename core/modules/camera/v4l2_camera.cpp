#include "v4l2_camera.hpp"

#include "../utils/configs.hpp"
#include "base_camera.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <linux/v4l2-controls.h>
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
namespace {

uint64_t v4l2_timestamp_to_ns(const v4l2_buffer& buffer) {
    constexpr uint64_t kNanosecondsPerSecond = 1'000'000'000ULL;
    constexpr uint64_t kNanosecondsPerMicrosecond = 1'000ULL;
    return (static_cast<uint64_t>(buffer.timestamp.tv_sec) * kNanosecondsPerSecond) +
           (static_cast<uint64_t>(buffer.timestamp.tv_usec) * kNanosecondsPerMicrosecond);
}

bool has_monotonic_v4l2_timestamp(const v4l2_buffer& buffer) {
#ifdef V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC
    return (buffer.flags & V4L2_BUF_FLAG_TIMESTAMP_MASK) == V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC;
#else
    static_cast<void>(buffer);
    return false;
#endif
}

} // namespace

const std::unordered_map<std::string, uint32_t> V4L2Camera::FOURCC_FORMATS = {
    {"NV16", V4L2_PIX_FMT_NV16},
    {"UYVY", V4L2_PIX_FMT_UYVY}
};

V4L2Camera::V4L2Camera(CameraConfig config) : Camera(std::move(config)) {
    _open_device();

    if (!is_valid()) {
        return;
    }

    _configure();

    if (!_init_mmap()) {
        return;
    }
}

void V4L2Camera::_post_stop() {
    if (is_valid()) {
        for (const auto buffer : _buffers) {
            munmap(buffer.start, buffer.length);
        }
        _close_device();
    }
}

bool V4L2Camera::_start_acquisition() {
    v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(_file_desc, VIDIOC_STREAMON, &type) < 0) {
        spdlog::warn("Failed to start streaming: {}, {}", get_config().device, errno);
        return false;
    }
    return true;
}

bool V4L2Camera::_stop_acquisition() {
    v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(_file_desc, VIDIOC_STREAMOFF, &type) < 0) {
        spdlog::warn("Failed to stop streaming: {}, {}", get_config().device, errno);
        return false;
    }
    return true;
}


bool V4L2Camera::_open_device() {
    if (is_valid()) {
        spdlog::info("Camera already open: {}", get_config().device);
        return true;
    }

    if (!std::filesystem::exists(get_config().device)) {
        spdlog::warn("Camera device does not exist: {}", get_config().device);
        return false;
    }

    _file_desc = open(get_config().device.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC, 0);
    if (_file_desc < 0) {
        spdlog::warn("Failed to open camera device: {}, {}", get_config().device, errno);
        return false;
    }
    spdlog::info("Opened camera device: {}", get_config().device);
    return true;
}

bool V4L2Camera::_close_device() {
    if (!is_valid()) {
        spdlog::info("Camera device already closed: {}", get_config().device);
        return true;
    }

    if (close(_file_desc) < 0) {
        spdlog::warn("Failed to close camera device: {}, {}", get_config().device, errno);
        return false;
    }
    _file_desc = -1;
    spdlog::info("Closed camera device: {}", get_config().device);
    return true;
}

bool V4L2Camera::_configure() const {
    struct v4l2_format fmt{};
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

    fmt.fmt.pix.width = get_config().writer.width;
    fmt.fmt.pix.height = get_config().writer.height;
    fmt.fmt.pix.pixelformat = FOURCC_FORMATS.at(get_config().format);
    fmt.fmt.pix.field = V4L2_FIELD_NONE;

    if (ioctl(_file_desc, VIDIOC_S_FMT, &fmt) < 0) {
        spdlog::warn("Failed to set format for device: {}, {}", get_config().device, errno);
        return false;
    }
    spdlog::info("Configured camera device: {}", get_config().device);

    // Try to set FPS if requested via config
    if (get_config().fps > 0) {
        struct v4l2_streamparm sparm{};
        sparm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

        // First query current parameters and capabilities
        if (ioctl(_file_desc, VIDIOC_G_PARM, &sparm) == 0) {
            if ((sparm.parm.capture.capability & V4L2_CAP_TIMEPERFRAME) == 0) {
                spdlog::info("Driver does not expose timeperframe; cannot set FPS for device: {}", get_config().device);
            } else {
                // timeperframe = numerator/denominator seconds per frame; FPS = denominator/numerator
                sparm.parm.capture.timeperframe.numerator = 1;
                sparm.parm.capture.timeperframe.denominator = get_config().fps; // integer FPS from config

                if (ioctl(_file_desc, VIDIOC_S_PARM, &sparm) < 0) {
                    spdlog::warn("Failed to set FPS={} on device: {}, {}", get_config().fps, get_config().device, errno);
                } else {
                    // Read back actual value the driver accepted
                    if (ioctl(_file_desc, VIDIOC_G_PARM, &sparm) == 0 && sparm.parm.capture.timeperframe.numerator != 0) {
                        const double actual_fps = static_cast<double>(sparm.parm.capture.timeperframe.denominator) / static_cast<double>(sparm.parm.capture.timeperframe.numerator);
                        spdlog::info("Requested FPS={} -> actual {:.2f} for device: {}", get_config().fps, actual_fps, get_config().device);
                    } else {
                        spdlog::info("Requested FPS={} for device: {} (could not verify actual)", get_config().fps, get_config().device);
                    }
                }
            }
        } else {
            spdlog::warn("VIDIOC_G_PARM unsupported; cannot set FPS for device: {}, {}", get_config().device, errno);
        }
    }

    // Apply settings
    for (const auto& setting : get_config().settings) {
        // Shortcut: if a control id is provided, use it directly
        if (setting.id != 0) {
            struct v4l2_control ctrl {};
            ctrl.id = setting.id;
            ctrl.value = setting.value;

            if (ioctl(_file_desc, VIDIOC_S_CTRL, &ctrl) < 0) {
                spdlog::warn("Failed to set control id={:x} value={} on device: {}, {}", setting.id, setting.value, get_config().device, errno);
            } else {
                spdlog::info("Set control id={:x} value={} on device: {}", setting.id, setting.value, get_config().device);
            }
            continue;
        }

        // Fallback: discover control by name
        struct v4l2_queryctrl queryctrl {};

        memset(&queryctrl, 0, sizeof(queryctrl));
        strncpy(reinterpret_cast<char*>(queryctrl.name), setting.name.c_str(), sizeof(queryctrl.name) - 1);

        bool found = false;
        for (queryctrl.id = V4L2_CID_BASE; queryctrl.id < V4L2_CID_LASTP1; queryctrl.id++) {
            if (ioctl(_file_desc, VIDIOC_QUERYCTRL, &queryctrl) == 0) {
                std::string ctrl_name = _get_ctrl_name(queryctrl.id);
                spdlog::info("Queried control '{}' (id={:x})", ctrl_name, queryctrl.id);
                if (setting.name == ctrl_name) {
                    found = true;
                    spdlog::info("Found control '{}' (id={:x}) for device: {}", ctrl_name, queryctrl.id, get_config().device);
                    break;
                }
            }
        }
        if (!found) {
            spdlog::warn("Control '{}' not found on device: {}", setting.name, get_config().device);
            continue;
        }

        struct v4l2_control ctrl {};
        ctrl.id = queryctrl.id;
        ctrl.value = setting.value;

        if (ioctl(_file_desc, VIDIOC_S_CTRL, &ctrl) < 0) {
            spdlog::warn("Failed to set control '{}'={} on device: {}, {}", setting.name, setting.value, get_config().device, errno);
        } else {
            spdlog::info("Set control '{}'={} on device: {}", setting.name, setting.value, get_config().device);
        }
    }
    return true;
}

std::string V4L2Camera::_get_ctrl_name(uint32_t ctrl_id) const {
    struct v4l2_queryctrl queryctrl {};
    queryctrl.id = ctrl_id;

    if (ioctl(_file_desc, VIDIOC_QUERYCTRL, &queryctrl) == 0) {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
        std::string ctrl_name(reinterpret_cast<const char*>(queryctrl.name));
        std::transform(ctrl_name.begin(), ctrl_name.end(), ctrl_name.begin(), ::tolower);
        std::replace(ctrl_name.begin(), ctrl_name.end(), ' ', '_');
        ctrl_name.erase(std::remove(ctrl_name.begin(), ctrl_name.end(), ','), ctrl_name.end());
        spdlog::info("Queried control name '{}' for id={:x} on device: {}", ctrl_name, ctrl_id, get_config().device);
        return ctrl_name;
    }

    spdlog::warn("Failed to query control name for id={:x} on device: {}, {}", ctrl_id, get_config().device, errno);
    return "";
}

bool V4L2Camera::_init_mmap() {
    struct v4l2_requestbuffers req{};
    req.count = get_config().req_buffer_count;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;

    if (ioctl(_file_desc, VIDIOC_REQBUFS, &req) < 0) {
        spdlog::warn("Failed to request buffers for device: {}, {}", get_config().device, errno);
        return false;
    }

    _buffers.resize(req.count);
    for (size_t index = 0; index < req.count; ++index) {
        struct v4l2_buffer buf{};
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = index;

        if (ioctl(_file_desc, VIDIOC_QUERYBUF, &buf) < 0) {
            spdlog::warn("Failed to query buffer for device: {}, {}", get_config().device, errno);
            return false;
        }

        _buffers[index].length = buf.length;
        _buffers[index].start = mmap(
            nullptr,
            buf.length,
            PROT_READ | PROT_WRITE,
            MAP_SHARED,
            _file_desc,
            buf.m.offset
        );

        // Queue buffer for capture
        if (ioctl(_file_desc, VIDIOC_QBUF, &buf) < 0) {
            spdlog::warn("Failed to queue buffer for device: {}, {}", get_config().device, errno);
            return false;
        }
    }

    return true;
}

void V4L2Camera::_capture_loop() {
    struct pollfd pfd{};
    pfd.fd = _file_desc;
    pfd.events = POLLIN;

    const uint32_t subsample = get_config().subsample_factor > 0 ? get_config().subsample_factor : 1;
    while (is_running()) {
        int const ret = poll(&pfd, 1, -1);
        if (ret < 0) {
            spdlog::warn("Poll error for device: {}, {}", get_config().device, errno);
            break;
        }

        struct v4l2_buffer buf{};
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;

        if (ioctl(_file_desc, VIDIOC_DQBUF, &buf) == 0) {
            const bool keep = (subsample == 1) || ((buf.sequence % subsample) == 0);

            if (keep) {
                const bool use_v4l2_timestamp = has_monotonic_v4l2_timestamp(buf);
                if (!use_v4l2_timestamp) {
                    spdlog::warn("Skipping camera frame for {} without monotonic V4L2 timestamp", get_config().device);
                    continue;
                }

                process_frame(_buffers[buf.index].start, buf.bytesused, buf.sequence, v4l2_timestamp_to_ns(buf));
            }

            if (ioctl(_file_desc, VIDIOC_QBUF, &buf) < 0) {
                spdlog::warn("Failed to re-queue buffer for device: {}, {}", get_config().device, errno);
                break;
            }
        } else {
            spdlog::warn("Failed to dequeue buffer for device: {}, {}", get_config().device, errno);
            break;
        }
    }
}

} // namespace core
