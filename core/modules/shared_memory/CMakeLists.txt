project(shared_memory LANGUAGES CXX)

add_library(shared_memory SHARED shared_dict_client.cpp)
target_include_directories(shared_memory PUBLIC ${CMAKE_SOURCE_DIR}/core/utils)

target_link_libraries(shared_memory PUBLIC core_utils_configs)
target_link_libraries(shared_memory PUBLIC spdlog::spdlog)
target_compile_options(core_utils_configs PRIVATE -fPIC) # TODO: WHy is this needed?

# if(ENABLE_TESTING)
#     add_executable(test_shared_memory tests/test_shared_memory.cpp)

#     set_target_properties(test_shared_memory PROPERTIES
#         RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests"
#     )

#     target_link_libraries(test_shared_memory
#                           gtest_main
#                           shared_memory)

#     add_test(NAME SharedMemoryTest COMMAND test_shared_memory)
# endif()
