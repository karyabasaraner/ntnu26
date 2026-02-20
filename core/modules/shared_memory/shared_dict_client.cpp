#include "shared_dict_client.hpp"
#include "../utils/configs.hpp"

#include <cstdint>
#include <mutex>
#include <spdlog/spdlog.h>
#include <string>

#include <cuda_runtime.h>
#include <nppi_color_conversion.h>

#include <fstream>

namespace core {

SharedDictClient::SharedDictClient() {
    _start_processing();
}

SharedDictClient::~SharedDictClient() {
    _stop_processing();
    _cv.notify_all();
    if (_worker_thread.joinable()) {
        _worker_thread.join();
    }
}

void SharedDictClient::_start_processing() {
    spdlog::info("Starting SharedDictClient worker thread");
    _worker_thread = std::thread(&SharedDictClient::_process_queue, this);
}

void SharedDictClient::_stop_processing() {
    spdlog::info("Stopping SharedDictClient worker thread");
    {
        std::lock_guard<std::mutex> lock(_queue_mutex);
        _stop = true;
    }
    _cv.notify_all();
}

void SharedDictClient::add(const std::string& key, const void* data, size_t length, uint64_t timestamp_ns) {
    // NOTE: This blocks the caller, so no heavy processing here!
    if (length > 0) {
        if (data == nullptr) {
            spdlog::warn("Attempted to add data with non-zero length but null pointer for key: {}", key);
            return;
        }

        std::vector<uint8_t> buf(length);
        std::memcpy(buf.data(), data, length);

        // Enqueue the data for processing
        DataEntry entry{key, std::move(buf), timestamp_ns};
        {
            std::lock_guard<std::mutex> lock(_queue_mutex);
            if (_stop) {
                spdlog::warn("Stopped, not adding data: {}", key);
                return;
            }

            _data_queue.push(std::move(entry));
        }

        // Notify the worker thread that we have new data
        _cv.notify_one();

    } else {
        spdlog::warn("Attempted to add data with zero length for key: {}", key);
    }
}

void SharedDictClient::_tmp_cuda_convert(std::vector<uint8_t>& data) {
    uint8_t* d_src;
    uint8_t* d_dst;

    size_t src_size = _config.width * _config.height * 2;   // UYVY = 16 bits per pixel
    size_t dst_size = _config.width * _config.height * 3;   // RGB

    cudaMalloc(&d_src, src_size);
    cudaMalloc(&d_dst, dst_size);

    cudaMemcpy(d_src, data.data(), data.size(), cudaMemcpyHostToDevice);

    auto start_time = std::chrono::steady_clock::now();
    NppiSize roi{_config.width, _config.height};

    // Convert from UYVY to RGB using NPP
    nppiYCbCr422ToRGB_8u_C2C3R(d_src, _config.width * 2, d_dst, _config.width * 3, roi);
    auto end_time = std::chrono::steady_clock::now();
    auto duration_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end_time - start_time).count();
    float duration_ms = duration_ns / 1e6f;
    spdlog::info("CUDA conversion took {} ms", duration_ms);

    auto ts = std::chrono::duration_cast<std::chrono::nanoseconds>(
              std::chrono::steady_clock::now().time_since_epoch()).count();
    std::string out_path = "/workspaces/core/converted_" + std::to_string(ts) + ".ppm";

    // write PPM (P6) header + raw RGB bytes
    std::vector<uint8_t> h_dst(dst_size);
    cudaMemcpy(h_dst.data(), d_dst, dst_size, cudaMemcpyDeviceToHost);
    std::ofstream ofs(out_path, std::ios::binary);
    if (ofs) {
        ofs << "P6\n" << _config.width << ' ' << _config.height << "\n255\n";
        ofs.write(reinterpret_cast<char*>(h_dst.data()), h_dst.size());
        ofs.close();
        spdlog::info("Wrote converted image to {}", out_path);
    } else {
        spdlog::warn("Failed to open {} for writing", out_path);
    }

    // cleanup
    cudaFree(d_src);
    cudaFree(d_dst);
}

void SharedDictClient::_process_queue() {
    // NOTE: (Post-) processing can be done here
    while(true) {
        DataEntry entry;
        {
            // 1. Acquire lock and wait for data (or stop)
            std::unique_lock<std::mutex> lock(_queue_mutex);
            _cv.wait(lock, [this] { return _stop || !_data_queue.empty(); });

            // 2. Check if we should stop
            if (_stop && _data_queue.empty()) {
                spdlog::info("Stopping SharedDictClient worker thread");
                return;
            }

            // 3. We have data to process, pop it from the queue
            entry = std::move(_data_queue.front());
            _data_queue.pop();

            // 4. Convert img data to RGB
            _tmp_cuda_convert(entry.data);
        }

        // TODO(MJ): Implement actual shared memory writing here!
        spdlog::info("Processing entry with key: {}, data length: {}, timestamp: {}", entry.key, entry.data.size(), entry.timestamp_ns);
    }
}

} // namespace core
