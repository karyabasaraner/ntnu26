include(FetchContent)

# yaml-cpp via FetchContent, and expose it to find_package(yaml-cpp)
set(YAML_CPP_INSTALL ON CACHE BOOL "Enable yaml-cpp install/export targets" FORCE)
FetchContent_Declare(
  yaml-cpp
  GIT_REPOSITORY git@github.com:marcojob/yaml-cpp.git
  GIT_TAG master
  OVERRIDE_FIND_PACKAGE
)
FetchContent_MakeAvailable(yaml-cpp)

# Keep third-party extras off by default
set(CONFIG_UTILS_BUILD_DEMOS OFF CACHE BOOL "Disable config_utilities demos" FORCE)
add_subdirectory(${CMAKE_SOURCE_DIR}/third-party/config_utilities/config_utilities)
