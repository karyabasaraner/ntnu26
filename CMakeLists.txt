cmake_minimum_required(VERSION 3.22)

project(Core LANGUAGES CXX)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Set a global C++ standard
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Set default build type if not specified
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

# Set optimization and debug flags
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG")
set(CMAKE_CXX_FLAGS_DEBUG "-O0 -g")

# Required dependencies
find_package(spdlog REQUIRED)
find_package(Threads REQUIRED)

# Debug builds has debug logging
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    add_definitions(-DSPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_DEBUG)
endif()

# Third-party dependencies
include(${CMAKE_SOURCE_DIR}/cmake/third_party.cmake)

# Core/modules
add_subdirectory(core/utils)
add_subdirectory(core/modules/camera)
add_subdirectory(core/modules/imu)
add_subdirectory(core/modules/shared_memory)
add_subdirectory(core/python/bindings)

# Testing Setup
if(ENABLE_TESTING)
    message(STATUS "Testing enabled")
    include(${CMAKE_SOURCE_DIR}/cmake/testing.cmake)
endif()

# Builds the service binary from core/start.cpp
add_executable(core-main core/start.cpp)

# Shared-memory MCAP logger sidecar
add_executable(core-log core/log.cpp)

# Provide root include for headers included like "modules/..."
target_include_directories(core-main PRIVATE ${CMAKE_SOURCE_DIR}/core)
target_include_directories(core-log PRIVATE ${CMAKE_SOURCE_DIR}/core)

# Link against the module libraries (camera_module pulls shared_memory transitively)
target_link_libraries(core-main PRIVATE camera_module imu_module Threads::Threads)
target_link_libraries(core-log PRIVATE shared_memory Threads::Threads)

# Optional: put binary at build/ instead of nested dir
set_target_properties(core-main PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)
set_target_properties(core-log PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)
