project(camera_module LANGUAGES CXX)

# Camera module
add_library(camera_module SHARED camera_module.cpp camera.cpp)
target_include_directories(camera_module PUBLIC ${CMAKE_SOURCE_DIR}/core/utils)

target_link_libraries(camera_module PUBLIC core_utils_configs)
target_compile_options(core_utils_configs PRIVATE -fPIC) # TODO: WHy is this needed?

if(ENABLE_TESTING)
    add_executable(test_camera_module tests/test_camera_module.cpp)

    set_target_properties(test_camera_module PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests"
    )

    target_link_libraries(test_camera_module
                          gtest_main
                          camera_module)

    add_test(NAME CameraModuleTest COMMAND test_camera_module)
endif()
