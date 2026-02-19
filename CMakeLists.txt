cmake_minimum_required(VERSION 3.28)

project(Core LANGUAGES CXX)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Set a global C++ standard
set(CMAKE_CXX_STANDARD 17)
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

# Third-party dependencies
include(${CMAKE_SOURCE_DIR}/cmake/third_party.cmake)

# Core/modules
add_subdirectory(core/utils)
add_subdirectory(core/modules/camera)
add_subdirectory(core/modules/shared_memory)

# Testing Setup
if(ENABLE_TESTING)
    message(STATUS "Testing enabled")
    include(${CMAKE_SOURCE_DIR}/cmake/testing.cmake)
endif()
