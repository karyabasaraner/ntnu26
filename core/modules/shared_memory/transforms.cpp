#include "transforms.hpp"

#include "../utils/configs.hpp"
#include "utils.hpp"

#include <cstddef>
#include <cstdint>
#include <cuda_runtime.h>
#include <cuda_runtime_api.h>
#include <driver_types.h>
#include <memory>
#include <nppdefs.h>
#include <nppi_color_conversion.h>
#include <spdlog/spdlog.h>
#include <utility>
#include <vector>

namespace core {

UYVY2RGB::UYVY2RGB(int width, int height,
                   TransformConfig config,  std::unique_ptr<Transform> wrapped_transform) :
                   Transform(width, height), _config(std::move(config)), _wrapped_transform(std::move(wrapped_transform))
{
    _allocate_cuda_buffers();
}

UYVY2RGB::~UYVY2RGB() {
    if (_cuda_src_buf != nullptr) {
        cudaFree(_cuda_src_buf);
    }
    if (_cuda_dst_buf != nullptr) {
        cudaFree(_cuda_dst_buf);
    }
}

void UYVY2RGB::apply(DataEntry &entry) {
    if (_wrapped_transform != nullptr) {
        _wrapped_transform->apply(entry);
    }
    spdlog::info("Applying UYVY2RGB transform to entry: {}", entry.key);

    const size_t expected_src = static_cast<size_t>(_get_width()) * static_cast<size_t>(_get_height()) * 2u;
    if (entry.data.size() != expected_src) {
        spdlog::error("UYVY2RGB: unexpected input size {} (expected {})", entry.data.size(), expected_src);
        return;
    }

    // 1. Copy input data to device
    cudaError_t cerr = cudaMemcpy(_cuda_src_buf, entry.data.data(), expected_src, cudaMemcpyHostToDevice);
    if (cerr != cudaSuccess) {
        spdlog::error("cudaMemcpy H2D failed: {}", cudaGetErrorString(cerr));
        return;
    }

    const int srcStep = _get_width() * 2; // bytes-per-row
    const int dstStep = _get_width() * 3; // bytes-per-row

    // 2. Apply the UYVY to RGB conversion using NPP
    NppStatus nstatus = nppiYCbCr422ToRGB_8u_C2C3R(
        static_cast<const Npp8u*>(_cuda_src_buf),
        srcStep,
        static_cast<Npp8u*>(_cuda_dst_buf),
        dstStep,
        _nppi_roi);

    if (nstatus != NPP_SUCCESS) {
        spdlog::error("nppiYCbCr422ToRGB failed: {}", nstatus);
        return;
    }

    // 3. Copy the result back to host
    std::vector<uint8_t> rgb_data(_dst_size);
    cerr = cudaMemcpy(rgb_data.data(), _cuda_dst_buf, _dst_size, cudaMemcpyDeviceToHost);
    if (cerr != cudaSuccess) {
        spdlog::error("cudaMemcpy D2H failed: {}", cudaGetErrorString(cerr));
        return;
    }
    entry.data = std::move(rgb_data);
}

void UYVY2RGB::_allocate_cuda_buffers() {
    const auto width = static_cast<size_t>(_get_width());
    const auto height = static_cast<size_t>(_get_height());
    const size_t src_size = width * height * 2u;
    _dst_size = width * height * 3u;

    if (cudaMalloc(&_cuda_src_buf, src_size) != cudaSuccess || cudaMalloc(&_cuda_dst_buf, _dst_size) != cudaSuccess) {
        spdlog::error("cudaMalloc failed");
    }

    _nppi_roi = { _get_width(), _get_height() };
}

} // namespace core