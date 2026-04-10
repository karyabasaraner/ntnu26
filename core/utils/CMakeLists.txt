add_library(core_utils_configs STATIC configs.cpp)
target_include_directories(core_utils_configs PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(core_utils_configs PUBLIC config_utilities::config_utilities)

if(ENABLE_TESTING)
    add_executable(test_core_configs
                   tests/test_configs.cpp)

    set_target_properties(test_core_configs PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests"
    )

    target_link_libraries(test_core_configs
                          gtest_main
                          core_utils_configs)

    add_test(NAME CoreConfigsTest COMMAND test_core_configs)
endif()
