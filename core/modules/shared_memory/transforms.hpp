#ifndef WORKSPACES_CORE_CORE_MODULES_SHARED_MEMORY_TRANSFORMS_HPP
#define WORKSPACES_CORE_CORE_MODULES_SHARED_MEMORY_TRANSFORMS_HPP

#include "utils.hpp"
#include "../utils/configs.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <nppdefs.h>

namespace core {

class Transform {
public:
    Transform(int width, int height) : _width(width), _height(height) {}
    virtual ~Transform() = default;
    Transform (const Transform&) = delete;
    Transform& operator=(const Transform&) = delete;
    Transform (Transform&&) = delete;
    Transform& operator=(Transform&&) = delete;

    virtual void apply(DataEntry &entry) {};

protected:
    int _get_width() const { return _width; }
    int _get_height() const { return _height; }

private:
    int _width;
    int _height;
};

class UYVY2RGB : public Transform {
public:
    UYVY2RGB(int width, int height, TransformConfig config, std::unique_ptr<Transform> wrapped_transform);
    ~UYVY2RGB() override;

    UYVY2RGB (const UYVY2RGB&) = delete;
    UYVY2RGB& operator=(const UYVY2RGB&) = delete;
    UYVY2RGB (UYVY2RGB&&) = delete;
    UYVY2RGB& operator=(UYVY2RGB&&) = delete;

    void apply(DataEntry &entry) override;

private:
    NppiSize _nppi_roi{0, 0};
    size_t _dst_size{0};
    std::unique_ptr<Transform> _wrapped_transform;
    TransformConfig _config;

    uint8_t* _cuda_src_buf = nullptr;
    uint8_t* _cuda_dst_buf = nullptr;

    void _allocate_cuda_buffers();
};

} // namespace core

#endif
