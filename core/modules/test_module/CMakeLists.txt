project(test_module LANGUAGES CXX)

# Test module
add_library(test_module SHARED test_module.cpp)
target_include_directories(test_module PUBLIC ${CMAKE_SOURCE_DIR}/core/utils)

# Eigen dependency
find_package(Eigen3 REQUIRED)
target_link_libraries(test_module PRIVATE Eigen3::Eigen)

# Test module testing
if(ENABLE_TESTING)
    add_executable(test_test_module tests/test_test_module.cpp)
    target_link_libraries(test_test_module
                          gtest_main
                          test_module)

    add_test(NAME TestModuleTest COMMAND test_test_module)
endif()
