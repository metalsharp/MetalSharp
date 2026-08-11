#include <metalsharp/ExtraShims.h>
#include <metalsharp/Kernel32Shim.h>
#include <metalsharp/Win32Types.h>

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace {

using metalsharp::ShimLibrary;
using metalsharp::win32::BOOL;
using metalsharp::win32::DWORD;
using metalsharp::win32::ERROR_BUFFER_OVERFLOW;
using metalsharp::win32::ERROR_ENVVAR_NOT_FOUND;

using GetEnvironmentVariableA = DWORD(MSABI*)(const char*, char*, DWORD);
using GetEnvironmentVariableW = DWORD(MSABI*)(const wchar_t*, wchar_t*, DWORD);
using SetEnvironmentVariableA = BOOL(MSABI*)(const char*, const char*);
using SetEnvironmentVariableW = BOOL(MSABI*)(const wchar_t*, const wchar_t*);
using GetLastError = DWORD(MSABI*)();

struct Kernel32EnvironmentApi {
    ShimLibrary library;
    GetEnvironmentVariableA getA;
    GetEnvironmentVariableW getW;
    SetEnvironmentVariableA setA;
    SetEnvironmentVariableW setW;
    GetLastError getLastError;
};

Kernel32EnvironmentApi makeApi() {
    Kernel32EnvironmentApi api;
    api.library = metalsharp::win32::Kernel32Shim::create();
    metalsharp::win32::addMissingKernel32(api.library);
    api.getA = reinterpret_cast<GetEnvironmentVariableA>(api.library.functions.at("GetEnvironmentVariableA")());
    api.getW = reinterpret_cast<GetEnvironmentVariableW>(api.library.functions.at("GetEnvironmentVariableW")());
    api.setA = reinterpret_cast<SetEnvironmentVariableA>(api.library.functions.at("SetEnvironmentVariableA")());
    api.setW = reinterpret_cast<SetEnvironmentVariableW>(api.library.functions.at("SetEnvironmentVariableW")());
    api.getLastError = reinterpret_cast<GetLastError>(api.library.functions.at("GetLastError")());
    return api;
}

bool expect(bool condition, const char* message) {
    if (!condition)
        std::cerr << "FAIL: " << message << '\n';
    return condition;
}

bool testCaseInsensitiveHostLookup(Kernel32EnvironmentApi& api) {
    constexpr const char* hostName = "MetalSharpIssue421HostCase";
    constexpr const char* hostValue = "host-case-value";
    setenv(hostName, hostValue, 1);

    char aBuffer[64] = {};
    wchar_t wBuffer[64] = {};
    DWORD aLength = api.getA("METALSHARPISSUE421HOSTCASE", aBuffer, sizeof(aBuffer));
    DWORD wLength = api.getW(L"metalsharpissue421hostcase", wBuffer, sizeof(wBuffer) / sizeof(wBuffer[0]));

    bool result = true;
    result &= expect(aLength == strlen(hostValue), "A lookup finds mixed-case host environment names");
    result &= expect(strcmp(aBuffer, hostValue) == 0, "A lookup returns the host value");
    result &= expect(wLength == strlen(hostValue), "W lookup finds mixed-case host environment names");
    result &= expect(std::wstring(wBuffer) == L"host-case-value", "W lookup returns the host value");

    unsetenv(hostName);
    return result;
}

bool testCanonicalSetAndCrossEncoding(Kernel32EnvironmentApi& api) {
    constexpr const char* name = "MetalSharpIssue421SetCase";
    bool result = true;
    result &= expect(api.setW(L"MetalSharpIssue421SetCase", L"set-by-w"), "SetEnvironmentVariableW succeeds");

    char aBuffer[64] = {};
    DWORD aLength = api.getA("metalsharpissue421setcase", aBuffer, sizeof(aBuffer));
    result &= expect(aLength == strlen("set-by-w"), "A reads a W-set variable case-insensitively");
    result &= expect(strcmp(aBuffer, "set-by-w") == 0, "A reads the W-set value");

    result &= expect(api.setA("METALSHARPISSUE421SETCASE", "set-by-a"), "SetEnvironmentVariableA succeeds");
    wchar_t wBuffer[64] = {};
    DWORD wLength = api.getW(L"metalsharpissue421setcase", wBuffer, sizeof(wBuffer) / sizeof(wBuffer[0]));
    result &= expect(wLength == strlen("set-by-a"), "W reads an A-set variable case-insensitively");
    result &= expect(std::wstring(wBuffer) == L"set-by-a", "W reads the A-set value");

    // The Windows-side map must not create or delete a POSIX variable.
    result &= expect(getenv(name) == nullptr, "Windows-side set does not leak into the POSIX environment");
    result &= expect(api.setW(L"metalsharpissue421setcase", nullptr), "SetEnvironmentVariableW deletes the variable");
    result &= expect(api.getA("METALSHARPISSUE421SETCASE", aBuffer, sizeof(aBuffer)) == 0,
                     "Deleted variables are not returned");
    result &= expect(api.getLastError() == ERROR_ENVVAR_NOT_FOUND, "Deleted variables report not found");
    return result;
}

bool testSmallBufferContract(Kernel32EnvironmentApi& api) {
    constexpr const char* value = "0123456789";
    bool result = true;
    result &= expect(api.setA("METALSHARPISSUE421BUFFER", value), "Set buffer test variable");

    char aBuffer[sizeof("0123456789")] = {};
    memset(aBuffer, 'x', sizeof(aBuffer));
    DWORD aRequired = api.getA("metalsharpissue421buffer", nullptr, 0);
    result &= expect(aRequired == strlen(value) + 1, "A zero-sized query returns the required size");
    DWORD aResult = api.getA("metalsharpissue421buffer", aBuffer, aRequired - 1);
    result &= expect(aResult == 0, "A short buffer returns zero");
    result &= expect(api.getLastError() == ERROR_BUFFER_OVERFLOW, "A short buffer reports ERROR_BUFFER_OVERFLOW");
    result &= expect(aBuffer[0] == 'x', "A short buffer is left untouched");

    wchar_t wBuffer[sizeof("0123456789")] = {};
    for (wchar_t& character : wBuffer)
        character = L'x';
    DWORD wRequired = api.getW(L"METALSHARPISSUE421BUFFER", nullptr, 0);
    result &= expect(wRequired == strlen(value) + 1, "W zero-sized query returns the required size");
    DWORD wResult = api.getW(L"METALSHARPISSUE421BUFFER", wBuffer, wRequired - 1);
    result &= expect(wResult == 0, "W short buffer returns zero");
    result &= expect(api.getLastError() == ERROR_BUFFER_OVERFLOW, "W short buffer reports ERROR_BUFFER_OVERFLOW");
    result &= expect(wBuffer[0] == L'x', "W short buffer is left untouched");

    std::vector<char> exactBuffer(aRequired);
    std::vector<wchar_t> exactWideBuffer(wRequired);
    result &= expect(api.getA("METALSHARPISSUE421BUFFER", exactBuffer.data(), aRequired) == strlen(value),
                     "A exact-sized buffer succeeds");
    result &= expect(api.getW(L"METALSHARPISSUE421BUFFER", exactWideBuffer.data(), wRequired) == strlen(value),
                     "W exact-sized buffer succeeds");
    result &= expect(strcmp(exactBuffer.data(), value) == 0, "A exact-sized buffer is null-terminated");
    result &= expect(std::wstring(exactWideBuffer.data()) == L"0123456789", "W exact-sized buffer is null-terminated");
    return result;
}

} // namespace

int main() {
    Kernel32EnvironmentApi api = makeApi();
    bool passed = true;
    passed &= testCaseInsensitiveHostLookup(api);
    passed &= testCanonicalSetAndCrossEncoding(api);
    passed &= testSmallBufferContract(api);

    api.setA("METALSHARPISSUE421BUFFER", nullptr);
    return passed ? 0 : 1;
}
