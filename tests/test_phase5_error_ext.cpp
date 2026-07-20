/// @file test_phase5_error_ext.cpp
/// @brief Phase 5a error tracker + 5b extension string tests.

#include <cstdio>
#include <cstring>
#include <metalsharp/GLErrorTracker.h>

extern "C" {
unsigned int glGetError();
const unsigned char* glGetString(unsigned int name);
}

static int passed = 0;
static int failed = 0;

#define CHECK(cond, msg)                                                                                               \
    do {                                                                                                               \
        if (cond) {                                                                                                    \
            printf("  [OK] %s\n", msg);                                                                                \
            passed++;                                                                                                  \
        } else {                                                                                                       \
            printf("  [FAIL] %s\n", msg);                                                                              \
            failed++;                                                                                                  \
        }                                                                                                              \
    } while (0)

int main() {
    printf("=== Phase 5 Error Tracker & Extensions Tests ===\n\n");

    {
        printf("--- GL error tracker ---\n");
        auto& tracker = metalsharp::GLErrorTracker::instance();
        (void)tracker.getError();
        CHECK(tracker.getError() == 0u, "Default error is GL_NO_ERROR (0)");
        tracker.setError(0x0500);
        CHECK(tracker.getError() == 0x0500u, "setError(GL_INVALID_ENUM) round-trips");
        CHECK(tracker.getError() == 0u, "getError clears the error");
        tracker.setError(0x0501);
        tracker.setError(0x0505);
        CHECK(tracker.getError() == 0x0501u, "First error wins");
        CHECK(tracker.getError() == 0u, "getError clears after first-error-wins");
        tracker.invalidEnum();
        CHECK(tracker.getError() == 0x0500u, "invalidEnum() = GL_INVALID_ENUM");
        tracker.invalidValue();
        CHECK(tracker.getError() == 0x0501u, "invalidValue() = GL_INVALID_VALUE");
        tracker.invalidOperation();
        CHECK(tracker.getError() == 0x0502u, "invalidOperation() = GL_INVALID_OPERATION");
        tracker.outOfMemory();
        CHECK(tracker.getError() == 0x0505u, "outOfMemory() = GL_OUT_OF_MEMORY");
    }

    {
        printf("\n--- glGetError integration ---\n");
        while (glGetError() != 0) {
        }
        metalsharp::GLErrorTracker::instance().invalidEnum();
        CHECK(glGetError() == 0x0500u, "glGetError() returns tracker error");
        CHECK(glGetError() == 0u, "glGetError() clears after read");
    }

    {
        printf("\n--- GL extension string ---\n");
        const unsigned int kGL_EXTENSIONS = 0x1F03;
        const unsigned char* extStr = glGetString(kGL_EXTENSIONS);
        CHECK(extStr != nullptr, "glGetString(GL_EXTENSIONS) non-null");
        if (extStr) {
            CHECK((reinterpret_cast<const char*>(extStr)[0] != '\0'), "glGetString(GL_EXTENSIONS) non-empty");
        }
    }

    printf("\n=== Summary: %d passed, %d failed ===\n", passed, failed);
    return failed ? 1 : 0;
}
