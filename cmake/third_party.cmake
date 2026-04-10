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
# Allow the caller to point CMake at a custom zstd install when the packaged config
# target cannot be found.
if(NOT DEFINED ZSTD_ROOT)
  set(ZSTD_ROOT "" CACHE PATH "Root directory of a zstd installation for manual discovery")
endif()


# TODO(MJ): This is quite a mess
find_package(zstd CONFIG QUIET)

if(TARGET zstd::libzstd_shared)
  message(STATUS "Using zstd config from package")
else()
  set(_ZSTD_SEARCH_PREFIXES ${CMAKE_PREFIX_PATH} ${CMAKE_INSTALL_PREFIX})
  if(ZSTD_ROOT)
    list(APPEND _ZSTD_SEARCH_PREFIXES ${ZSTD_ROOT})
  endif()

  find_path(_ZSTD_INCLUDE_DIR
    NAMES zstd.h
    PATH_SUFFIXES include include/zstd
    PATHS ${_ZSTD_SEARCH_PREFIXES}
  )

  find_library(_ZSTD_SHARED_LIBRARY
    NAMES zstd libzstd
    PATH_SUFFIXES lib lib64 lib/x86_64-linux-gnu lib64/x86_64-linux-gnu
    PATHS ${_ZSTD_SEARCH_PREFIXES}
  )

  if(_ZSTD_INCLUDE_DIR AND _ZSTD_SHARED_LIBRARY AND NOT TARGET zstd::libzstd_shared)
    add_library(zstd::libzstd_shared UNKNOWN IMPORTED GLOBAL)
    set_target_properties(zstd::libzstd_shared PROPERTIES
      IMPORTED_LOCATION "${_ZSTD_SHARED_LIBRARY}"
      INTERFACE_INCLUDE_DIRECTORIES "${_ZSTD_INCLUDE_DIR}"
    )
    message(STATUS "Found zstd library at ${_ZSTD_SHARED_LIBRARY}")
  endif()
endif()

if(NOT TARGET zstd::libzstd_shared)
  message(FATAL_ERROR "zstd not found. Install libzstd-dev or set ZSTD_ROOT to your zstd prefix.")
endif()

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

add_subdirectory(${CMAKE_SOURCE_DIR}/third-party/mcap_builder)
