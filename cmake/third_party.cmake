# Expose our custom modules whenever this file is included.
list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_LIST_DIR}/Modules)

# config_utilities
set(CONFIG_UTILS_BUILD_DEMOS OFF CACHE BOOL "Disable config_utilities demos" FORCE)
add_subdirectory(${CMAKE_SOURCE_DIR}/third-party/config_utilities/config_utilities)

# libiio
set(HAVE_DNS_SD OFF CACHE BOOL "Disable DNS-SD support in libiio" FORCE)
find_package(libaio REQUIRED)
set(HAVE_DNS_SD OFF)
add_subdirectory(${CMAKE_SOURCE_DIR}/third-party/libiio)

# MCAP
find_package(zstd REQUIRED CONFIG)

# Older Debian/Ubuntu configs only expose zstd::libzstd_shared, but mcap_builder
# looks for `zstd::libzstd`. Create an alias that mirrors the shared target so the
# dependency resolution succeeds without touching the upstream project.
if(TARGET zstd::libzstd_shared AND NOT TARGET zstd::libzstd)
  add_library(zstd::libzstd INTERFACE IMPORTED)
  set_target_properties(zstd::libzstd PROPERTIES
    INTERFACE_LINK_LIBRARIES "zstd::libzstd_shared"
    INTERFACE_INCLUDE_DIRECTORIES "$<TARGET_PROPERTY:zstd::libzstd_shared,INTERFACE_INCLUDE_DIRECTORIES>"
  )
endif()

include(FetchContent)
FetchContent_Declare(
  mcap_builder
  GIT_REPOSITORY git@github.com:marcojob/mcap_builder.git
  GIT_TAG main
)
FetchContent_MakeAvailable(mcap_builder)
