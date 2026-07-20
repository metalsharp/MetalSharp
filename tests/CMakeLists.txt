cmake_minimum_required(VERSION 3.24)

add_executable(test_metal_device
    test_metal_device.cpp
)
target_link_libraries(test_metal_device PRIVATE metalsharp_core)
target_compile_options(test_metal_device PRIVATE -fobjc-arc)

add_executable(test_format_translation
    test_format_translation.cpp
)
target_link_libraries(test_format_translation PRIVATE metalsharp_core)

add_executable(test_d3d11_device
    test_d3d11_device.cpp
)
target_link_libraries(test_d3d11_device PRIVATE metalsharp_d3d11 metalsharp_core)

add_executable(test_triangle
    test_triangle.mm
)
target_link_libraries(test_triangle PRIVATE metalsharp_d3d11 metalsharp_core)
target_compile_options(test_triangle PRIVATE -fobjc-arc)

add_executable(test_phase2
    test_phase2.mm
)
target_link_libraries(test_phase2 PRIVATE metalsharp_d3d11 metalsharp_core)
target_compile_options(test_phase2 PRIVATE -fobjc-arc)

add_executable(test_dxbc
    test_dxbc.cpp
)
target_link_libraries(test_dxbc PRIVATE metalsharp_core)

add_executable(test_deferred_context
    test_deferred_context.cpp
)
target_link_libraries(test_deferred_context PRIVATE metalsharp_d3d11 metalsharp_core)

add_executable(test_d3d12
    test_d3d12.cpp
)
target_link_libraries(test_d3d12 PRIVATE metalsharp_d3d12 metalsharp_core)

add_executable(test_d3d12_entrypoint
    test_d3d12_entrypoint.cpp
)
target_link_libraries(test_d3d12_entrypoint PRIVATE metalsharp_d3d12 metalsharp_core)

add_executable(test_runtime
    test_runtime.cpp
)
target_link_libraries(test_runtime PRIVATE metalsharp_core)

add_executable(test_host_runtime_abi
    test_host_runtime_abi.cpp
)
target_include_directories(test_host_runtime_abi PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(test_host_runtime_abi PRIVATE metalsharp_host_runtime)

add_executable(test_darwin_sync_map
    test_darwin_sync_map.cpp
)
target_link_libraries(test_darwin_sync_map PRIVATE metalsharp_core)

add_executable(test_audio
    test_audio.cpp
)
target_link_libraries(test_audio PRIVATE metalsharp_audio metalsharp_core)

add_executable(test_input
    test_input.cpp
)
target_link_libraries(test_input PRIVATE metalsharp_input metalsharp_core)
target_compile_options(test_input PRIVATE -fobjc-arc)

add_executable(test_phase5
    test_phase5.cpp
)
target_link_libraries(test_phase5 PRIVATE metalsharp_core)

add_executable(test_tiled_resources
    test_tiled_resources.cpp
)
target_link_libraries(test_tiled_resources PRIVATE metalsharp_core)

add_test(NAME metal_device COMMAND test_metal_device)
add_test(NAME format_translation COMMAND test_format_translation)
add_test(NAME d3d11_device COMMAND test_d3d11_device)
add_test(NAME triangle COMMAND test_triangle)
add_test(NAME phase2 COMMAND test_phase2)
add_test(NAME dxbc COMMAND test_dxbc)
add_test(NAME deferred_context COMMAND test_deferred_context)
add_test(NAME d3d12 COMMAND test_d3d12)
add_test(NAME d3d12_entrypoint COMMAND test_d3d12_entrypoint)
add_test(NAME runtime COMMAND test_runtime)
add_test(NAME host_runtime_abi COMMAND test_host_runtime_abi)
add_test(NAME darwin_sync_map COMMAND test_darwin_sync_map)
add_test(NAME audio COMMAND test_audio)
add_test(NAME input COMMAND test_input)
add_test(NAME phase5 COMMAND test_phase5)
add_test(NAME tiled_resources COMMAND test_tiled_resources)

add_executable(test_phase17
    test_phase17.cpp
)
target_link_libraries(test_phase17 PRIVATE metalsharp_core)
target_compile_options(test_phase17 PRIVATE -fobjc-arc)

add_test(NAME phase17 COMMAND test_phase17)

add_executable(test_phase18
    test_phase18.cpp
)
target_link_libraries(test_phase18 PRIVATE metalsharp_d3d12 metalsharp_core)
target_compile_options(test_phase18 PRIVATE -fobjc-arc)

add_test(NAME phase18 COMMAND test_phase18)

add_executable(test_phase19
    test_phase19.cpp
)
target_link_libraries(test_phase19 PRIVATE metalsharp_d3d12 metalsharp_core)
target_compile_options(test_phase19 PRIVATE -fobjc-arc)

add_test(NAME phase19 COMMAND test_phase19)

add_executable(test_phase20
    test_phase20.cpp
)
target_link_libraries(test_phase20 PRIVATE metalsharp_loader metalsharp_core)
target_compile_options(test_phase20 PRIVATE -fobjc-arc)

add_test(NAME phase20 COMMAND test_phase20)

add_executable(test_phase21
    test_phase21.cpp
)
target_link_libraries(test_phase21 PRIVATE metalsharp_core)
target_compile_options(test_phase21 PRIVATE -fobjc-arc)

add_test(NAME phase21 COMMAND test_phase21)

add_executable(test_phase22
    test_phase22.cpp
)
target_link_libraries(test_phase22 PRIVATE metalsharp_audio metalsharp_core)
target_compile_options(test_phase22 PRIVATE -fobjc-arc)

add_test(NAME phase22 COMMAND test_phase22)

add_executable(test_phase23
    test_phase23.cpp
)
target_link_libraries(test_phase23 PRIVATE metalsharp_core)
target_compile_options(test_phase23 PRIVATE -fobjc-arc)

add_test(NAME phase23 COMMAND test_phase23)

add_executable(test_phase24
    test_phase24.cpp
)
target_link_libraries(test_phase24 PRIVATE metalsharp_core)
target_compile_options(test_phase24 PRIVATE -fobjc-arc)

add_test(NAME phase24 COMMAND test_phase24)

add_executable(test_roadmap
    test_roadmap.cpp
)
target_link_libraries(test_roadmap PRIVATE metalsharp_d3d12 metalsharp_core)

add_test(NAME roadmap COMMAND test_roadmap)

add_executable(test_spirv_cross_sanity
    test_spirv_cross_sanity.cpp
)
target_link_libraries(test_spirv_cross_sanity PRIVATE spirv-cross-c spirv-cross-glsl spirv-cross-msl spirv-cross-core)
add_test(NAME spirv_cross_sanity COMMAND test_spirv_cross_sanity)

add_executable(test_glsl_compiler
    test_glsl_compiler.cpp
)
target_link_libraries(test_glsl_compiler PRIVATE
    metalsharp_opengl32
    metalsharp_core
    glslang
    OSDependent
    SPIRV
    glslang-default-resource-limits
    spirv-cross-core
    spirv-cross-glsl
    spirv-cross-msl)
add_test(NAME glsl_compiler COMMAND test_glsl_compiler)

add_executable(test_metal_renderer
    test_metal_renderer.mm
)
target_link_libraries(test_metal_renderer PRIVATE metalsharp_opengl32 metalsharp_core ${METAL_FRAMEWORK})
target_compile_options(test_metal_renderer PRIVATE -fobjc-arc)
add_test(NAME metal_renderer COMMAND test_metal_renderer)

add_executable(test_wgl_context
    test_wgl_context.mm
)
target_link_libraries(test_wgl_context PRIVATE metalsharp_opengl32 metalsharp_core ${METAL_FRAMEWORK} ${APPKIT_FRAMEWORK})
target_compile_options(test_wgl_context PRIVATE -fobjc-arc)
add_test(NAME wgl_context COMMAND test_wgl_context)

add_executable(test_phase5_error_ext
    test_phase5_error_ext.cpp
)
target_link_libraries(test_phase5_error_ext PRIVATE metalsharp_opengl32 metalsharp_core)
add_test(NAME phase5_error_ext COMMAND test_phase5_error_ext)
