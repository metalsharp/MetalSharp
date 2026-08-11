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

add_executable(test_fna_kernel32_shim
    test_fna_kernel32_shim.c
    ../src/fna/shims/kernel32_shim.c
)

add_executable(test_thread_completion
    test_thread_completion.cpp
)
target_link_libraries(test_thread_completion PRIVATE metalsharp_loader metalsharp_core)

add_executable(test_host_runtime_abi
    test_host_runtime_abi.cpp
)
target_include_directories(test_host_runtime_abi PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(test_host_runtime_abi PRIVATE metalsharp_host_runtime)

add_executable(test_mscoree_path_safety
    test_mscoree_path_safety.cpp
)
target_include_directories(test_mscoree_path_safety PRIVATE ${CMAKE_SOURCE_DIR}/src/wine)

add_executable(test_darwin_sync_map
    test_darwin_sync_map.cpp
)
target_link_libraries(test_darwin_sync_map PRIVATE metalsharp_core)

add_executable(test_sync_context
    test_sync_context.cpp
)
target_link_libraries(test_sync_context PRIVATE metalsharp_core)

add_executable(test_win32_last_error
    test_win32_last_error.cpp
)
target_link_libraries(test_win32_last_error PRIVATE metalsharp_loader metalsharp_core)

add_executable(test_audio
    test_audio.cpp
)
target_link_libraries(test_audio PRIVATE metalsharp_audio metalsharp_core)

add_executable(test_input
    test_input.cpp
)
target_link_libraries(test_input PRIVATE metalsharp_input metalsharp_core)
target_compile_options(test_input PRIVATE -fobjc-arc)

add_executable(test_network_context
    test_network_context.cpp
)
target_link_libraries(test_network_context PRIVATE metalsharp_loader metalsharp_core)

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
add_test(NAME fna_kernel32_shim COMMAND test_fna_kernel32_shim)
add_test(NAME thread_completion COMMAND test_thread_completion)
add_test(NAME host_runtime_abi COMMAND test_host_runtime_abi)
add_test(NAME mscoree_path_safety COMMAND test_mscoree_path_safety)
add_test(NAME darwin_sync_map COMMAND test_darwin_sync_map)
add_test(NAME sync_context COMMAND test_sync_context)
add_test(NAME win32_last_error COMMAND test_win32_last_error)
add_test(NAME audio COMMAND test_audio)
add_test(NAME input COMMAND test_input)
add_test(NAME network_context COMMAND test_network_context)
add_test(NAME phase5 COMMAND test_phase5)
add_test(NAME tiled_resources COMMAND test_tiled_resources)
# Regression test for #441: the FNA kernel32 shim is macOS-only (it uses
# mach headers) and ships as a dylib compiled from src/fna/shims, so build
# it the same way and exercise GetModuleFileNameA through dlopen.
if(APPLE)
    add_library(kernel32_shim_testlib SHARED ../src/fna/shims/kernel32_shim.c)
    add_executable(test_kernel32_shim test_kernel32_shim.c)
    target_link_libraries(test_kernel32_shim PRIVATE ${CMAKE_DL_LIBS})
    add_test(NAME kernel32_shim COMMAND test_kernel32_shim $<TARGET_FILE:kernel32_shim_testlib>)
endif()

# Regression test for #427: the ws2_32 event-object shim (WSAWaitForMultipleEvents
# and friends) must wait on the real event handles/sockets, not poll stdin.
add_executable(test_ws2_32_events test_ws2_32_events.cpp)
target_link_libraries(test_ws2_32_events PRIVATE metalsharp_loader metalsharp_core)
add_test(NAME ws2_32_events COMMAND test_ws2_32_events)

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

# Winsock TIMEVAL regression test for the ws2_32 select shim
# (metalsharp/MetalSharp#423).
add_executable(test_ws2_select
    test_ws2_select.cpp
)
target_link_libraries(test_ws2_select PRIVATE metalsharp_loader metalsharp_core)
add_test(NAME ws2_select COMMAND test_ws2_select)

add_executable(test_resource_bounds
    test_resource_bounds.cpp
)
target_link_libraries(test_resource_bounds PRIVATE metalsharp_loader metalsharp_core)
target_compile_options(test_resource_bounds PRIVATE -fobjc-arc)

add_test(NAME resource_bounds COMMAND test_resource_bounds)

add_executable(test_pe_loader
    test_pe_loader.cpp
)
target_link_libraries(test_pe_loader PRIVATE metalsharp_loader metalsharp_core)
target_compile_options(test_pe_loader PRIVATE -fobjc-arc)

add_test(NAME pe_loader COMMAND test_pe_loader)

add_executable(test_pe_import_bounds
    test_pe_import_bounds.cpp
)
target_link_libraries(test_pe_import_bounds PRIVATE metalsharp_loader metalsharp_core)
add_test(NAME pe_import_bounds COMMAND test_pe_import_bounds)

add_executable(test_pe_loader_relocations
    test_pe_loader_relocations.cpp
)
target_link_libraries(test_pe_loader_relocations PRIVATE metalsharp_loader metalsharp_core)
add_test(NAME pe_loader_relocations COMMAND test_pe_loader_relocations)

add_executable(test_virtual_file_system
    test_virtual_file_system.cpp
)
target_link_libraries(test_virtual_file_system PRIVATE metalsharp_loader metalsharp_core)

add_test(NAME virtual_file_system COMMAND test_virtual_file_system)

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

add_executable(test_opengl_bridge
    test_opengl_bridge.cpp
)
target_link_libraries(test_opengl_bridge PRIVATE metalsharp_opengl32 metalsharp_core)
target_compile_options(test_opengl_bridge PRIVATE -fobjc-arc)
add_test(NAME opengl_bridge COMMAND test_opengl_bridge)

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

# Mach-port lifecycle regression test for the Linux EAC substrate
# (metalsharp/MetalSharp#442). The substrate is x86_64/Rosetta-only; the
# project default (Wine) architecture matches, as in CI.
add_executable(test_eac_substrate
    test_eac_substrate.c
)
target_compile_options(test_eac_substrate PRIVATE -Wno-deprecated-declarations)
# The substrate links the Wine-only ntdll_get_unix_file_name via weak
# dynamic lookup; keep the same link contract as metalsharp_eac_substrate.
target_link_options(test_eac_substrate PRIVATE "-Wl,-undefined,dynamic_lookup")
add_test(NAME eac_substrate COMMAND test_eac_substrate)

add_executable(test_kernel32_environment
    test_kernel32_environment.cpp
)
target_link_libraries(test_kernel32_environment PRIVATE metalsharp_loader metalsharp_core)
add_test(NAME kernel32_environment COMMAND test_kernel32_environment)

add_executable(test_kernel32
    test_kernel32.cpp
)
target_link_libraries(test_kernel32 PRIVATE metalsharp_loader metalsharp_core)
add_test(NAME kernel32 COMMAND test_kernel32)

# Regression coverage for #418: kernel32 wide-char entry points must convert
# UTF-16 arguments instead of reading them as ANSI (Kernel32Shim.cpp). The
# loader library contains the kernel32 shims (Kernel32Shim + Kernel32Extra).
add_executable(test_kernel32_wide
    test_kernel32_wide.cpp
)
target_link_libraries(test_kernel32_wide PRIVATE metalsharp_loader metalsharp_core)
add_test(NAME kernel32_wide COMMAND test_kernel32_wide)

# The mscoree PE shim is built for Wine's x86_64 lane outside the host-native
# targets above. Exercise its host-side unixlib contract here with a small
# fake Mono dylib: the regression must use the PE-provided executable path and
# copy Mono's result back into the shared launch-parameter structure.
add_library(test_mscoree_mono SHARED
    test_mscoree_mono.c
)

add_executable(test_mscoree_unix
    test_mscoree_unix.c
    ${CMAKE_SOURCE_DIR}/src/wine/mscoree_unix.c
)
target_include_directories(test_mscoree_unix PRIVATE ${CMAKE_SOURCE_DIR}/src/wine)
target_link_libraries(test_mscoree_unix PRIVATE ${CMAKE_DL_LIBS})
add_dependencies(test_mscoree_unix test_mscoree_mono)
add_test(NAME mscoree_unix COMMAND test_mscoree_unix)
set_tests_properties(mscoree_unix PROPERTIES
    ENVIRONMENT "METALSHARP_MONO_LIB=$<TARGET_FILE:test_mscoree_mono>"
)
