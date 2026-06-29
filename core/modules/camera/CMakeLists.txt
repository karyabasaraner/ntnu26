project(camera_module LANGUAGES CXX)

# Camera module
set(CAMERA_SOURCES
    base_camera.cpp
    camera_factory.cpp
    camera_module.cpp
    v4l2_camera.cpp)

if(ENABLE_PYLON)
    cmake_policy(PUSH)
    if(POLICY CMP0144)
        cmake_policy(SET CMP0144 NEW)
    endif()
    list(APPEND CMAKE_PREFIX_PATH "$ENV{PYLON_ROOT}")
    find_package(pylon REQUIRED CONFIG)
    cmake_policy(POP)
    list(APPEND CAMERA_SOURCES pylon_camera.cpp)
endif()

add_library(camera_module SHARED ${CAMERA_SOURCES})
target_include_directories(camera_module PUBLIC ${CMAKE_SOURCE_DIR}/core/utils)

target_link_libraries(camera_module PUBLIC core_utils_configs)
target_link_libraries(camera_module PUBLIC spdlog::spdlog)
target_link_libraries(camera_module PUBLIC shared_memory)
if(ENABLE_PYLON)
    target_compile_definitions(camera_module PUBLIC CORE_ENABLE_PYLON)
    target_link_libraries(camera_module PRIVATE pylon::pylon)
endif()
target_compile_options(core_utils_configs PRIVATE -fPIC) # TODO: WHy is this needed?

if(ENABLE_TESTING)
    add_executable(test_camera_module
                   tests/test_base_camera.cpp
                   tests/test_camera_factory.cpp
                   tests/test_camera_module.cpp)

    set_target_properties(test_camera_module PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests"
    )

    target_link_libraries(test_camera_module
                          gtest_main
                          camera_module
                          shared_memory)

    add_test(NAME CameraModuleTest COMMAND test_camera_module)
endif()
