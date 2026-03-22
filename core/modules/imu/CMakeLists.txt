project(imu_module LANGUAGES CXX)

# IMU module
add_library(imu_module SHARED imu_module.cpp)
target_include_directories(imu_module PUBLIC ${CMAKE_SOURCE_DIR}/core/utils)

target_link_libraries(imu_module PUBLIC core_utils_configs)
target_link_libraries(imu_module PUBLIC spdlog::spdlog)
target_link_libraries(imu_module PUBLIC shared_memory)
target_compile_options(core_utils_configs PRIVATE -fPIC) # TODO: Why is this needed?
