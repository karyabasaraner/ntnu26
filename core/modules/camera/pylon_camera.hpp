#ifndef WORKSPACES_CORE_CORE_MODULES_CAMERA_PYLON_CAMERA_HPP
#define WORKSPACES_CORE_CORE_MODULES_CAMERA_PYLON_CAMERA_HPP

#include "base_camera.hpp"
#include "configs.hpp"

#include <memory>

namespace core {

class PylonCamera final : public Camera {
public:
    explicit PylonCamera(CameraConfig config);
    ~PylonCamera() override;

    PylonCamera(const PylonCamera&) = delete;
    PylonCamera& operator=(const PylonCamera&) = delete;
    PylonCamera(PylonCamera&&) = delete;
    PylonCamera& operator=(PylonCamera&&) = delete;

    bool is_valid() const override;

private:
    struct Impl;
    std::unique_ptr<Impl> _impl;

    static std::unique_ptr<Impl> _create_impl();
    bool _start_acquisition() override;
    bool _stop_acquisition() noexcept override;
    void _capture_loop() override;
    void _post_stop() noexcept override;
};

} // namespace core

#endif
