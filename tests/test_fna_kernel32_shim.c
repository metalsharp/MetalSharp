#include <stdint.h>
#include <stdio.h>

int MultiByteToWideChar(uint32_t CodePage, uint32_t dwFlags, const char* lpMultiByteStr, int cbMultiByte,
                        void* lpWideCharStr, int cchWideChar);

static int passed;
static int failed;

#define CHECK(condition, message)                                                                                      \
    do {                                                                                                               \
        if (condition) {                                                                                               \
            printf("  [OK] %s\n", message);                                                                            \
            passed++;                                                                                                  \
        } else {                                                                                                       \
            printf("  [FAIL] %s\n", message);                                                                          \
            failed++;                                                                                                  \
        }                                                                                                              \
    } while (0)

int main(void) {
    const char input[] = "FNA";
    uint16_t output[sizeof(input) + 1];
    for (size_t i = 0; i < sizeof(output) / sizeof(output[0]); i++)
        output[i] = 0xA55A;

    int converted = MultiByteToWideChar(0, 0, input, 0, output, (int)sizeof(input));

    CHECK(converted == 3, "auto-length conversion reports payload length");
    CHECK(output[0] == 'F' && output[1] == 'N' && output[2] == 'A', "auto-length conversion copies input");
    CHECK(output[3] == 0, "auto-length conversion writes a UTF-16 terminator");
    CHECK(output[4] == 0xA55A, "terminator write stays within the requested buffer");

    printf("=== Results: %d passed, %d failed ===\n", passed, failed);
    return failed != 0;
}
