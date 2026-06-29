#ifdef CORE_ENABLE_PYLON

#include "pylon_camera.hpp"

#include "base_camera.hpp"
#include "configs.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>

#include <Base/GCException.h>
#include <GenApi/INodeMap.h>
#include <GenApi/Pointer.h>
#include <pylon/DeviceClass.h>
#include <pylon/DeviceInfo.h>
#include <pylon/ETimeoutHandling.h>
#include <pylon/GrabResultPtr.h>
#include <pylon/ImageFormatConverter.h>
#include <pylon/InstantCamera.h>
#include <pylon/PixelType.h>
#include <pylon/PylonBase.h>
#include <pylon/PylonImage.h>
#include <pylon/TlFactory.h>
#include <spdlog/spdlog.h>

namespace core {
namespace {

class PylonRuntime final {
public:
    PylonRuntime() { Pylon::PylonInitialize(); }
    ~PylonRuntime() { Pylon::PylonTerminate(); }

    PylonRuntime(const PylonRuntime&) = delete;
    PylonRuntime& operator=(const PylonRuntime&) = delete;
    PylonRuntime(PylonRuntime&&) = delete;
    PylonRuntime& operator=(PylonRuntime&&) = delete;
};

PylonRuntime& pylon_runtime() {
    static PylonRuntime runtime;
    return runtime;
}

void set_enumeration(GenApi::INodeMap& node_map, const char* name, const std::string& value) {
    GenApi::CEnumerationPtr const node(node_map.GetNode(name));
    if (!node || !GenApi::IsWritable(node)) {
        spdlog::warn("Pylon parameter '{}' is not writable; keeping the camera default", name);
        return;
    }
    GenApi::CEnumEntryPtr const entry(node->GetEntryByName(value.c_str()));
    if (!entry || !GenApi::IsReadable(entry)) {
        spdlog::warn("Pylon parameter '{}' does not support value '{}'; keeping the camera default", name, value);
        return;
    }
    node->SetIntValue(entry->GetValue());
}

void set_boolean(GenApi::INodeMap& node_map, const char* name, bool value) {
    GenApi::CBooleanPtr const node(node_map.GetNode(name));
    if (node && GenApi::IsWritable(node)) {
        node->SetValue(value);
    }
}

void set_integer(GenApi::INodeMap& node_map, const char* name, int64_t value) {
    GenApi::CIntegerPtr const node(node_map.GetNode(name));
    if (!node || !GenApi::IsWritable(node)) {
        spdlog::warn("Pylon parameter '{}' is not writable; keeping the camera default", name);
        return;
    }
    node->SetValue(std::clamp(
        value,
        node->GetMin(),
        node->GetMax()
    ));
}

void set_float(GenApi::INodeMap& node_map, const char* name, double value) {
    GenApi::CFloatPtr const node(node_map.GetNode(name));
    if (!node || !GenApi::IsWritable(node)) {
        spdlog::warn("Pylon parameter '{}' is not writable; keeping the camera default", name);
        return;
    }
    node->SetValue(std::clamp(value, node->GetMin(), node->GetMax()));
}

uint64_t steady_time_ns() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()
    ).count());
}

} // namespace

struct PylonCamera::Impl {
    Pylon::CInstantCamera camera;
    Pylon::CImageFormatConverter converter;
    Pylon::CPylonImage converted_image;
};

std::unique_ptr<PylonCamera::Impl> PylonCamera::_create_impl() {
    static_cast<void>(pylon_runtime());
    return std::make_unique<Impl>();
}

PylonCamera::PylonCamera(CameraConfig config) : Camera(std::move(config)), _impl(_create_impl()) {
    const PylonCameraConfig& pylon_config = get_config().pylon;

    Pylon::CDeviceInfo device_info;
    device_info.SetDeviceClass(Pylon::BaslerGigEDeviceClass);
    device_info.SetIpAddress(pylon_config.ip_address.c_str());
    device_info.SetPortNr("3956");

    _impl->camera.Attach(Pylon::CTlFactory::GetInstance().CreateDevice(device_info));
    _impl->camera.Open();

    GenApi::INodeMap& node_map = _impl->camera.GetNodeMap();
    set_enumeration(node_map, "AcquisitionMode", "Continuous");
    set_enumeration(node_map, "PixelFormat", pylon_config.format);
    set_integer(node_map, "Width", static_cast<int64_t>(get_config().writer.width));
    set_integer(node_map, "Height", static_cast<int64_t>(get_config().writer.height));
    set_boolean(node_map, "AcquisitionFrameRateEnable", true);
    set_float(node_map, "AcquisitionFrameRate", static_cast<double>(get_config().fps));

    if (pylon_config.exposure_time_us > 0.0) {
        set_enumeration(node_map, "ExposureAuto", "Off");
        set_float(node_map, "ExposureTime", pylon_config.exposure_time_us);
    }
    if (pylon_config.gain > 0.0) {
        set_enumeration(node_map, "GainAuto", "Off");
        set_float(node_map, "Gain", pylon_config.gain);
    }
    GenApi::INodeMap& stream_node_map = _impl->camera.GetStreamGrabberNodeMap();
    if (pylon_config.packet_size > 0) {
        set_integer(stream_node_map, "GevSCPSPacketSize", pylon_config.packet_size);
    }
    if (pylon_config.inter_packet_delay > 0) {
        set_integer(stream_node_map, "GevSCPD", pylon_config.inter_packet_delay);
    }

    _impl->converter.OutputPixelFormat = Pylon::PixelType_RGB8packed;
    _impl->converter.OutputPaddingX = 0;
    spdlog::info("Opened Pylon GigE camera '{}' at {}", get_config().name, pylon_config.ip_address);
}

PylonCamera::~PylonCamera() {
    stop();
}

bool PylonCamera::is_valid() const {
    return _impl && _impl->camera.IsOpen() && !_impl->camera.IsCameraDeviceRemoved();
}

bool PylonCamera::_start_acquisition() {
    try {
        _impl->camera.StartGrabbing(Pylon::GrabStrategy_LatestImageOnly, Pylon::GrabLoop_ProvidedByUser);
        return true;
    } catch (const GenICam::GenericException& error) {
        spdlog::error("Failed to start Pylon camera '{}': {}", get_config().name, error.GetDescription());
        return false;
    }
}

bool PylonCamera::_stop_acquisition() noexcept {
    try {
        if (_impl && _impl->camera.IsGrabbing()) {
            _impl->camera.StopGrabbing();
        }
        return true;
    } catch (const GenICam::GenericException& error) {
        spdlog::error("Failed to stop Pylon camera '{}': {}", get_config().name, error.GetDescription());
        return false;
    }
}

void PylonCamera::_capture_loop() {
    constexpr unsigned int GRAB_TIMEOUT_MS = 100;
    const uint32_t subsample = std::max(get_config().subsample_factor, 1U);

    while (is_running() && _impl->camera.IsGrabbing()) {
        try {
            Pylon::CGrabResultPtr grab_result;
            if (!_impl->camera.RetrieveResult(GRAB_TIMEOUT_MS, grab_result, Pylon::TimeoutHandling_Return)) {
                continue;
            }
            if (!grab_result->GrabSucceeded()) {
                spdlog::warn(
                    "Pylon grab failed for '{}': {} ({})",
                    get_config().name,
                    grab_result->GetErrorDescription().c_str(),
                    grab_result->GetErrorCode()
                );
                continue;
            }

            const uint64_t block_id = grab_result->GetBlockID();
            if (subsample > 1U && (block_id % subsample) != 0U) {
                continue;
            }

            _impl->converter.Convert(_impl->converted_image, grab_result);
            process_frame(
                _impl->converted_image.GetBuffer(),
                _impl->converted_image.GetImageSize(),
                static_cast<uint32_t>(block_id),
                steady_time_ns()
            );
        } catch (const GenICam::GenericException& error) {
            spdlog::error("Pylon acquisition failed for '{}': {}", get_config().name, error.GetDescription());
            break;
        }
    }
}

void PylonCamera::_post_stop() noexcept {
    try {
        if (_impl && _impl->camera.IsOpen()) {
            _impl->camera.Close();
        }
    } catch (const GenICam::GenericException& error) {
        spdlog::error("Failed to close Pylon camera '{}': {}", get_config().name, error.GetDescription());
    }
}

} // namespace core

#endif
