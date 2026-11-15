cmake_minimum_required(VERSION 3.24)

add_executable(test_host_runtime_abi
    test_host_runtime_abi.cpp
)
target_include_directories(test_host_runtime_abi PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(test_host_runtime_abi PRIVATE metalsharp_host_runtime)
metalsharp_host_target(test_host_runtime_abi)

add_test(NAME host_runtime_abi COMMAND test_host_runtime_abi)
