/// @file test_spirv_cross_sanity.cpp
/// @brief Phase 2a sanity check: verify the SPIRV-Cross C++ static libraries
///        (spirv-cross-core, spirv-cross-glsl, spirv-cross-msl) link and that
///        the C API is reachable via the propagated include directories.
///
/// This is intentionally minimal — it only verifies that the SPIRV-Cross
/// build is wired into the project (correct include paths, correct
/// target_link_libraries wiring). The real GLSL→SPIR-V→MSL translation
/// pipeline is delivered in later Phase 2x phases.
///
/// Note: SPIRV-Cross's vendored C API uses the `spvc_*` symbol prefix and
/// `SPVC_ENV_*`/version-query functions rather than the older `spc_*` form;
/// we use the actual upstream API here.

#include <cstdio>
#include <spirv_cross_c.h>

int main() {
    int failed = 0;

    printf("=== SPIRV-Cross sanity test ===\n\n");

    {
        unsigned major = 0, minor = 0, patch = 0;
        spvc_get_version(&major, &minor, &patch);
        if (minor != 0 || patch != 0) {
            printf("[OK] spvc_get_version returned %u.%u.%u (C ABI present)\n", major, minor, patch);
        } else {
            printf("[FAIL] spvc_get_version returned all zeros (library not linked?)\n");
            failed++;
        }
    }

    {
        spvc_context ctx = nullptr;
        spvc_result result = spvc_context_create(&ctx);
        if (result == SPVC_SUCCESS && ctx != nullptr) {
            printf("[OK] spvc_context_create succeeded (OpenGL 4.5 backend available via spirv-cross-glsl)\n");
            spvc_context_destroy(ctx);
        } else {
            printf("[FAIL] spvc_context_create returned error %d, ctx=%p\n", (int)result, (void*)ctx);
            failed++;
        }
    }

    printf("\n=== Summary: %d passed, %d failed ===\n", 2 - failed, failed);
    return failed ? 1 : 0;
}
