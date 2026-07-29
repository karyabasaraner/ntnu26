project(event_camera_module LANGUAGES CXX)

# Event camera module
set(EVENT_CAMERA_SOURCES
    event_camera.cpp
    event_camera_factory.cpp
    event_camera_module.cpp)

if(ENABLE_METAVISION)
    find_package(MetavisionSDK COMPONENTS core driver base REQUIRED)
    list(APPEND EVENT_CAMERA_SOURCES prophesee_event_camera.cpp)
endif()

add_library(event_camera_module SHARED ${EVENT_CAMERA_SOURCES})
target_include_directories(event_camera_module PUBLIC ${CMAKE_SOURCE_DIR}/core/utils)

target_link_libraries(event_camera_module PUBLIC core_utils_configs)
target_link_libraries(event_camera_module PUBLIC spdlog::spdlog)
target_link_libraries(event_camera_module PUBLIC shared_memory)
if(ENABLE_METAVISION)
    target_compile_definitions(event_camera_module PUBLIC CORE_ENABLE_METAVISION)
    target_link_libraries(event_camera_module PRIVATE MetavisionSDK::core MetavisionSDK::driver MetavisionSDK::base)
endif()
target_compile_options(core_utils_configs PRIVATE -fPIC) # TODO: Why is this needed?

if(ENABLE_TESTING)
    add_executable(test_event_camera_module
                   tests/test_base_event_camera.cpp
                   tests/test_event_camera_factory.cpp
                   tests/test_event_camera_module.cpp)

    set_target_properties(test_event_camera_module PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests"
    )

    target_link_libraries(test_event_camera_module
                          gtest_main
                          event_camera_module
                          shared_memory)

    add_test(NAME EventCameraModuleTest COMMAND test_event_camera_module)
endif()
