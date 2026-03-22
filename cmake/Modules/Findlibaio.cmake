# Custom Finder for libaio when no config package is provided in the distro.
find_path(libaio_INCLUDE_DIR
  NAMES aio.h
  HINTS ${libaio_ROOT_DIR} ${libaio_DIR} ${CMAKE_PREFIX_PATH}
  PATH_SUFFIXES include
)
find_library(libaio_LIBRARY
  NAMES aio
  HINTS ${libaio_ROOT_DIR} ${libaio_DIR} ${CMAKE_PREFIX_PATH}
  PATH_SUFFIXES lib lib64 aarch64-linux-gnu arm-linux-gnueabihf
)
include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(libaio DEFAULT_MSG libaio_LIBRARY libaio_INCLUDE_DIR)
if(libaio_FOUND)
  set(libaio_LIBRARIES ${libaio_LIBRARY})
  set(libaio_INCLUDE_DIRS ${libaio_INCLUDE_DIR})
endif()
mark_as_advanced(libaio_INCLUDE_DIR libaio_LIBRARY)
