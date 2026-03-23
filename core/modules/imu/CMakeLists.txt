project(imu_module LANGUAGES CXX)

# IMU module
add_library(imu_module SHARED imu_module.cpp imu_device.cpp)
target_include_directories(imu_module PUBLIC ${CMAKE_SOURCE_DIR}/core/utils)

target_link_libraries(imu_module PUBLIC core_utils_configs)
target_link_libraries(imu_module PUBLIC spdlog::spdlog)
target_link_libraries(imu_module PUBLIC shared_memory)
target_link_libraries(imu_module PUBLIC iio)
target_compile_options(core_utils_configs PRIVATE -fPIC) # TODO: Why is this needed?

if(ENABLE_TESTING)
    add_executable(test_imu_module tests/test_imu_module.cpp)

    set_target_properties(test_imu_module PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests"
    )

    target_link_libraries(test_imu_module
                          gtest_main
                          imu_module
                          shared_memory)

    add_test(NAME IMUModuleTest COMMAND test_imu_module)
endif()
