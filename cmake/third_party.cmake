# Expose our custom modules whenever this file is included.
list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_LIST_DIR}/Modules)

# config_utilities
set(CONFIG_UTILS_BUILD_DEMOS OFF CACHE BOOL "Disable config_utilities demos" FORCE)
add_subdirectory(${CMAKE_SOURCE_DIR}/third-party/config_utilities/config_utilities)

# libiio
find_package(libaio REQUIRED)
set(HAVE_DNS_SD OFF)
add_subdirectory(${CMAKE_SOURCE_DIR}/third-party/libiio)
