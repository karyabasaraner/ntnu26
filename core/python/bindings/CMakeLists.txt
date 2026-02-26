include(FetchContent)
FetchContent_Declare(
  nanobind
  GIT_REPOSITORY https://github.com/wjakob/nanobind.git
  GIT_TAG        v2.0.0
)
FetchContent_MakeAvailable(nanobind)

# Require Python development headers for nanobind (>=3.9)
find_package(Python 3.9 COMPONENTS Interpreter Development.Module REQUIRED)
if(NOT TARGET Python::Module)
  find_package(Python3 3.9 COMPONENTS Interpreter Development.Module QUIET)
  if(TARGET Python3::Module AND NOT TARGET Python::Module)
    add_library(Python::Module INTERFACE IMPORTED)
    target_link_libraries(Python::Module INTERFACE Python3::Module)
  endif()
endif()

nanobind_add_module(core_sharedmem MODULE STABLE_ABI reader_bindings.cpp)
target_include_directories(core_sharedmem PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(core_sharedmem PRIVATE shared_memory camera_module spdlog::spdlog)

# Place the Python extension next to other build outputs for easy import during dev
set_target_properties(core_sharedmem PROPERTIES LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
