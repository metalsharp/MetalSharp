#include <array>
#include <cstdio>
#include <metalsharp/Kernel32Shim.h>
#include <metalsharp/Win32Types.h>

using metalsharp::ShimLibrary;
using metalsharp::win32::BOOL;
using metalsharp::win32::DWORD;
using metalsharp::win32::Kernel32Shim;
using metalsharp::win32::UINT;

using MultiByteToWideCharFn = int(MSABI*)(UINT, DWORD, const char*, int, wchar_t*, int);
using WideCharToMultiByteFn = int(MSABI*)(UINT, DWORD, const wchar_t*, int, char*, int, const char*, BOOL*);

static int testsPassed = 0;
static int testsFailed = 0;

#define TEST(name)                                                                                                     \
    printf("  TEST: %-55s", #name);                                                                                    \
    if (test_##name()) {                                                                                               \
        printf("PASS\n");                                                                                              \
        testsPassed++;                                                                                                 \
    } else {                                                                                                           \
        printf("FAIL\n");                                                                                              \
        testsFailed++;                                                                                                 \
    }

static MultiByteToWideCharFn getMultiByteToWideChar() {
    ShimLibrary library = Kernel32Shim::create();
    return reinterpret_cast<MultiByteToWideCharFn>(library.functions.at("MultiByteToWideChar")());
}

static WideCharToMultiByteFn getWideCharToMultiByte() {
    ShimLibrary library = Kernel32Shim::create();
    return reinterpret_cast<WideCharToMultiByteFn>(library.functions.at("WideCharToMultiByte")());
}

static bool test_multibyte_null_terminator_and_size() {
    const char input[] = "abc";
    auto convert = getMultiByteToWideChar();

    int required = convert(65001, 0, input, -1, nullptr, 0);
    std::array<wchar_t, 4> output = {L'?', L'?', L'?', L'?'};
    int written = convert(65001, 0, input, -1, output.data(), static_cast<int>(output.size()));

    return required == 4 && written == 4 && output[0] == L'a' && output[1] == L'b' && output[2] == L'c' &&
           output[3] == L'\0';
}

static bool test_multibyte_explicit_length_stays_unterminated() {
    const char input[] = "abc";
    auto convert = getMultiByteToWideChar();
    std::array<wchar_t, 4> output = {L'?', L'?', L'?', L'?'};

    int written = convert(65001, 0, input, 3, output.data(), static_cast<int>(output.size()));

    return written == 3 && output[0] == L'a' && output[1] == L'b' && output[2] == L'c' && output[3] == L'?';
}

static bool test_wide_null_terminator_and_size() {
    const wchar_t input[] = L"abc";
    auto convert = getWideCharToMultiByte();

    int required = convert(65001, 0, input, -1, nullptr, 0, nullptr, nullptr);
    std::array<char, 4> output = {'?', '?', '?', '?'};
    int written = convert(65001, 0, input, -1, output.data(), static_cast<int>(output.size()), nullptr, nullptr);

    return required == 4 && written == 4 && output[0] == 'a' && output[1] == 'b' && output[2] == 'c' &&
           output[3] == '\0';
}

static bool test_wide_explicit_length_stays_unterminated() {
    const wchar_t input[] = L"abc";
    auto convert = getWideCharToMultiByte();
    std::array<char, 4> output = {'?', '?', '?', '?'};

    int written = convert(65001, 0, input, 3, output.data(), static_cast<int>(output.size()), nullptr, nullptr);

    return written == 3 && output[0] == 'a' && output[1] == 'b' && output[2] == 'c' && output[3] == '?';
}

static bool test_zero_lengths_are_rejected() {
    const char narrowInput[] = "abc";
    const wchar_t wideInput[] = L"abc";
    auto multiByteToWide = getMultiByteToWideChar();
    auto wideToMultiByte = getWideCharToMultiByte();
    std::array<wchar_t, 4> wideOutput{};
    std::array<char, 4> narrowOutput{};

    return multiByteToWide(65001, 0, narrowInput, 0, wideOutput.data(), static_cast<int>(wideOutput.size())) == 0 &&
           wideToMultiByte(65001, 0, wideInput, 0, narrowOutput.data(), static_cast<int>(narrowOutput.size()), nullptr,
                           nullptr) == 0;
}

int main() {
    printf("=== Kernel32 String Conversion Tests ===\n\n");

    TEST(multibyte_null_terminator_and_size);
    TEST(multibyte_explicit_length_stays_unterminated);
    TEST(wide_null_terminator_and_size);
    TEST(wide_explicit_length_stays_unterminated);
    TEST(zero_lengths_are_rejected);

    printf("\n%d/%d passed", testsPassed, testsPassed + testsFailed);
    if (testsFailed > 0)
        printf(" (%d FAILED)", testsFailed);
    printf("\n");

    return testsFailed > 0 ? 1 : 0;
}
