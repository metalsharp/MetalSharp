cmake_minimum_required(VERSION 3.24)
set(CMAKE_OSX_DEPLOYMENT_TARGET "14.0" CACHE STRING "Minimum macOS deployment target" FORCE)
project(metalsharp VERSION 1.5.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(METALSHARP_HOST_ARCH "arm64" CACHE STRING "Architecture for macOS host-side targets")

function(metalsharp_host_target target)
    set_target_properties(${target} PROPERTIES OSX_ARCHITECTURES "${METALSHARP_HOST_ARCH}")
endfunction()

# The host runtime ABI library the runtime installs to runtime/host
# (tools/package/create-host-runtime.sh stages it into app/native/host).
add_library(metalsharp_host_runtime SHARED
    src/runtime/host/HostRuntimeABI.cpp
)
target_include_directories(metalsharp_host_runtime PUBLIC include)
set_target_properties(metalsharp_host_runtime PROPERTIES OUTPUT_NAME "metalsharp_host_runtime")
metalsharp_host_target(metalsharp_host_runtime)

if(BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
