cmake_minimum_required(VERSION 3.24)

if(NOT APPLE)
    message(STATUS "macOS architecture matrix skipped on non-Apple host")
    return()
endif()

find_program(LIPO_EXECUTABLE lipo REQUIRED)

function(assert_architecture label path expected)
    if(NOT EXISTS "${path}")
        message(FATAL_ERROR "${label} is missing: ${path}")
    endif()

    execute_process(
        COMMAND "${LIPO_EXECUTABLE}" -info "${path}"
        RESULT_VARIABLE info_result
        OUTPUT_VARIABLE info_output
        ERROR_VARIABLE info_error
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_STRIP_TRAILING_WHITESPACE
    )
    if(NOT info_result EQUAL 0)
        message(FATAL_ERROR "lipo -info failed for ${label}: ${info_error}")
    endif()

    execute_process(
        COMMAND "${LIPO_EXECUTABLE}" -archs "${path}"
        RESULT_VARIABLE arch_result
        OUTPUT_VARIABLE arch_output
        ERROR_VARIABLE arch_error
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_STRIP_TRAILING_WHITESPACE
    )
    if(NOT arch_result EQUAL 0)
        message(FATAL_ERROR "lipo -archs failed for ${label}: ${arch_error}")
    endif()

    if(NOT "${arch_output}" STREQUAL "${expected}")
        message(FATAL_ERROR
            "${label} has architecture '${arch_output}', expected '${expected}'; ${info_output}")
    endif()
    message(STATUS "${label}: ${info_output}")
endfunction()

assert_architecture("metalsharp_host_runtime" "${HOST_RUNTIME}" "${METALSHARP_HOST_ARCH}")
assert_architecture("test_host_runtime_abi" "${HOST_RUNTIME_TEST}" "${METALSHARP_HOST_ARCH}")
assert_architecture("metalsharp_launcher" "${HOST_LAUNCHER}" "${METALSHARP_HOST_ARCH}")
assert_architecture("metalsharp_migrator" "${HOST_MIGRATOR}" "${METALSHARP_HOST_ARCH}")

assert_architecture("metalsharp_core" "${WINE_CORE}" "${METALSHARP_WINE_ARCH}")
assert_architecture("metalsharp_native" "${WINE_NATIVE}" "${METALSHARP_WINE_ARCH}")
assert_architecture("metalsharp_d3d11" "${WINE_D3D11}" "${METALSHARP_WINE_ARCH}")
assert_architecture("metalsharp_d3d12" "${WINE_D3D12}" "${METALSHARP_WINE_ARCH}")
assert_architecture("metalsharp_dxgi" "${WINE_DXGI}" "${METALSHARP_WINE_ARCH}")
assert_architecture("metalsharp_audio" "${WINE_AUDIO}" "${METALSHARP_WINE_ARCH}")
assert_architecture("metalsharp_input" "${WINE_INPUT}" "${METALSHARP_WINE_ARCH}")
assert_architecture("metalsharp_loader" "${WINE_LOADER}" "${METALSHARP_WINE_ARCH}")
assert_architecture("metalsharp_opengl32" "${WINE_OPENGL}" "${METALSHARP_WINE_ARCH}")
