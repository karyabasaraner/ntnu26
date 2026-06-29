#ifndef WORKSPACES_CORE_CORE_MODULES_CAMERA_V4L2_CAMERA_HPP
#define WORKSPACES_CORE_CORE_MODULES_CAMERA_V4L2_CAMERA_HPP

#include "../utils/configs.hpp"
#include "base_camera.hpp"


#include <cstddef>
#include <cstdint>
#include <string>

#include <unordered_map>
#include <vector>

namespace core {

class V4L2Camera : public Camera {
public:
    explicit V4L2Camera(CameraConfig config);

    // Delete copy and move
    V4L2Camera(const V4L2Camera&) = delete;
    V4L2Camera& operator=(const V4L2Camera&) = delete;
    V4L2Camera(V4L2Camera&&) = delete;
    V4L2Camera& operator=(V4L2Camera&&) = delete;

    ~V4L2Camera() override;

    bool is_valid() const override { return _file_desc >= 0; };

private:
    int _file_desc = -1;
    static const std::unordered_map<std::string, uint32_t> FOURCC_FORMATS;

    // Buffer struct for memory mapping
    struct Buffer {
        void* start;
        size_t length;
    };
    std::vector<Buffer> _buffers;

    bool _start_acquisition() override;
    bool _stop_acquisition() noexcept override;
    void _capture_loop() override;
    void _post_stop() noexcept override;

    bool _close_device();
    bool _configure() const;
    bool _configure_format() const;
    void _configure_fps() const;
    void _configure_setting(const V4L2SettingConfig& setting) const;
    void _configure_settings() const;
    uint32_t _find_control_id(const std::string& name) const;
    bool _init_mmap();
    bool _open_device();
    std::string _get_ctrl_name(uint32_t ctrl_id) const;
};

} // namespace core

#endif
