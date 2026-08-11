#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <metalsharp/VirtualFileSystem.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

using metalsharp::win32::HANDLE;
using metalsharp::win32::INVALID_HANDLE_VALUE;
using metalsharp::win32::VirtualFileSystem;

namespace {

constexpr size_t kFindDataFileNameOffset = 44;
constexpr size_t kFindDataFileNameCapacity = 260;
constexpr size_t kFindDataSize =
    kFindDataFileNameOffset + kFindDataFileNameCapacity * sizeof(uint16_t) + 14 * sizeof(uint16_t);
constexpr uint32_t kReserved0 = 0x13579BDF;
constexpr uint32_t kReserved1 = 0x2468ACE0;

uint16_t readCodeUnit(const std::array<uint8_t, kFindDataSize>& data, size_t index) {
    uint16_t codeUnit = 0;
    memcpy(&codeUnit, data.data() + kFindDataFileNameOffset + index * sizeof(codeUnit), sizeof(codeUnit));
    return codeUnit;
}

std::string readFileName(const std::array<uint8_t, kFindDataSize>& data) {
    std::string name;
    for (size_t i = 0; i < kFindDataFileNameCapacity; i++) {
        uint16_t codeUnit = readCodeUnit(data, i);
        if (codeUnit == 0)
            break;
        if (codeUnit > 0x7F)
            return {};
        name.push_back(static_cast<char>(codeUnit));
    }
    return name;
}

bool hasZeroTail(const std::array<uint8_t, kFindDataSize>& data, size_t nameLength) {
    for (size_t i = nameLength + 1; i < kFindDataFileNameCapacity; i++) {
        if (readCodeUnit(data, i) != 0)
            return false;
    }
    return true;
}

} // namespace

int main() {
    int passed = 0;
    int failed = 0;

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

    printf("=== VirtualFileSystem Find Data Tests ===\n\n");

    char directoryTemplate[] = "/tmp/metalsharp-find-data-XXXXXX";
    char* directoryPath = mkdtemp(directoryTemplate);
    CHECK(directoryPath != nullptr, "Create temporary directory");
    if (!directoryPath)
        return 1;

    const std::array<const char*, 2> expectedNames = {{"config.ini", "long-file-name-for-find-data.ini"}};
    bool filesCreated = true;
    for (const char* name : expectedNames) {
        std::string path = std::string(directoryPath) + "/" + name;
        int fd = open(path.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0600);
        if (fd < 0) {
            filesCreated = false;
            continue;
        }
        constexpr char contents[] = "test";
        if (write(fd, contents, sizeof(contents) - 1) != static_cast<ssize_t>(sizeof(contents) - 1))
            filesCreated = false;
        close(fd);
    }
    CHECK(filesCreated, "Create find-data fixtures");

    std::string pattern = std::string(directoryPath) + "/*";
    std::array<uint8_t, kFindDataSize> findData;
    findData.fill(0xA5);
    memcpy(findData.data() + 36, &kReserved0, sizeof(kReserved0));
    memcpy(findData.data() + 40, &kReserved1, sizeof(kReserved1));

    HANDLE findHandle = VirtualFileSystem::instance().findFirstFileW(pattern.c_str(), findData.data());
    bool enumerationSucceeded = findHandle != INVALID_HANDLE_VALUE;
    bool reservedFieldsPreserved = true;
    bool configNameFound = false;
    bool longNameFound = false;
    bool namesAreComplete = true;

    while (enumerationSucceeded) {
        uint32_t reserved0 = 0;
        uint32_t reserved1 = 0;
        memcpy(&reserved0, findData.data() + 36, sizeof(reserved0));
        memcpy(&reserved1, findData.data() + 40, sizeof(reserved1));
        reservedFieldsPreserved &= reserved0 == kReserved0 && reserved1 == kReserved1;

        std::string name = readFileName(findData);
        if (name == expectedNames[0])
            configNameFound = true;
        if (name == expectedNames[1])
            longNameFound = true;
        if (name == expectedNames[0] || name == expectedNames[1])
            namesAreComplete &= hasZeroTail(findData, name.size());

        enumerationSucceeded = VirtualFileSystem::instance().findNextFileW(findHandle, findData.data()) != 0;
    }

    CHECK(findHandle != INVALID_HANDLE_VALUE, "FindFirstFile returns a handle");
    CHECK(configNameFound, "cFileName contains config.ini at offset 44");
    CHECK(longNameFound, "cFileName is not limited to the alternate-name length");
    CHECK(reservedFieldsPreserved, "dwReserved0 and dwReserved1 remain untouched");
    CHECK(namesAreComplete, "cFileName is zeroed after the terminator");
    if (findHandle != INVALID_HANDLE_VALUE)
        CHECK(VirtualFileSystem::instance().findClose(findHandle) != 0, "Close find handle");

    for (const char* name : expectedNames) {
        std::string path = std::string(directoryPath) + "/" + name;
        unlink(path.c_str());
    }
    rmdir(directoryPath);

    printf("\n=== Results: %d passed, %d failed ===\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
