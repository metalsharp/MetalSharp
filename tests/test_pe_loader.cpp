#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <metalsharp/PEHeader.h>
#include <metalsharp/PELoader.h>
#include <string>
#include <unistd.h>
#include <vector>

using namespace metalsharp;

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

namespace {

constexpr size_t kNtHeadersOffset = 0x80;
constexpr size_t kOptionalHeaderOffset = kNtHeadersOffset + sizeof(uint32_t) + sizeof(IMAGE_FILE_HEADER);
constexpr size_t kSectionTableOffset = kOptionalHeaderOffset + sizeof(IMAGE_OPTIONAL_HEADER64);
constexpr size_t kMinimumHeadersSize = 0x200;
constexpr size_t kMaxPEFileSize = 512ull * 1024ull * 1024ull;

std::vector<uint8_t> makeMinimalPE() {
    const size_t fileSize = std::max(kMinimumHeadersSize, kSectionTableOffset + sizeof(IMAGE_SECTION_HEADER));
    std::vector<uint8_t> data(fileSize);

    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(data.data());
    dos->e_magic = IMAGE_DOS_SIGNATURE;
    dos->e_lfanew = kNtHeadersOffset;

    auto* signature = reinterpret_cast<uint32_t*>(data.data() + kNtHeadersOffset);
    *signature = IMAGE_PE_SIGNATURE;

    auto* fileHeader = reinterpret_cast<IMAGE_FILE_HEADER*>(signature + 1);
    fileHeader->Machine = IMAGE_FILE_MACHINE_AMD64;
    fileHeader->NumberOfSections = 1;
    fileHeader->SizeOfOptionalHeader = sizeof(IMAGE_OPTIONAL_HEADER64);

    auto* optionalHeader = reinterpret_cast<IMAGE_OPTIONAL_HEADER64*>(data.data() + kOptionalHeaderOffset);
    optionalHeader->Magic = IMAGE_OPTIONAL_MAGIC_PE32PLUS;
    optionalHeader->ImageBase = 0x140000000ULL;
    optionalHeader->SectionAlignment = 0x1000;
    optionalHeader->FileAlignment = 0x200;
    optionalHeader->SizeOfImage = 0x2000;
    optionalHeader->SizeOfHeaders = kMinimumHeadersSize;

    auto* section = reinterpret_cast<IMAGE_SECTION_HEADER*>(data.data() + kSectionTableOffset);
    section->Name[0] = '.';
    section->Name[1] = 't';
    section->Name[2] = 'e';
    section->Name[3] = 's';
    section->Name[4] = 't';
    section->VirtualSize = 0x10;
    section->VirtualAddress = 0x1000;
    section->Characteristics = IMAGE_SCN_MEM_READ;

    return data;
}

IMAGE_FILE_HEADER* fileHeader(std::vector<uint8_t>& data) {
    return reinterpret_cast<IMAGE_FILE_HEADER*>(data.data() + kNtHeadersOffset + sizeof(uint32_t));
}

IMAGE_SECTION_HEADER* firstSection(std::vector<uint8_t>& data) {
    return reinterpret_cast<IMAGE_SECTION_HEADER*>(data.data() + kSectionTableOffset);
}

std::string temporaryPath(const char* suffix) {
    const char* tempDirectory = getenv("TMPDIR");
    return std::string(tempDirectory ? tempDirectory : "/tmp") + "/metalsharp-pe-loader-" + std::to_string(getpid()) +
           suffix;
}

static bool test_accepts_minimal_pe() {
    auto data = makeMinimalPE();
    PELoader loader;
    return loader.loadFromMemory(data.data(), data.size()) && loader.getBase() != nullptr;
}

static bool test_rejects_optional_header_past_file() {
    auto data = makeMinimalPE();
    data.resize(kOptionalHeaderOffset + sizeof(IMAGE_OPTIONAL_HEADER64));
    fileHeader(data)->SizeOfOptionalHeader = sizeof(IMAGE_OPTIONAL_HEADER64) + 1;

    PELoader loader;
    return !loader.loadFromMemory(data.data(), data.size());
}

static bool test_rejects_truncated_section_table() {
    auto data = makeMinimalPE();
    data.resize(kSectionTableOffset + sizeof(IMAGE_SECTION_HEADER));
    fileHeader(data)->NumberOfSections = 2;

    PELoader loader;
    return !loader.loadFromMemory(data.data(), data.size());
}

static bool test_rejects_raw_section_past_file() {
    auto data = makeMinimalPE();
    auto* section = firstSection(data);
    section->PointerToRawData = static_cast<uint32_t>(data.size() - 1);
    section->SizeOfRawData = 2;

    PELoader loader;
    return !loader.loadFromMemory(data.data(), data.size());
}

static bool test_rejects_virtual_section_past_image() {
    auto data = makeMinimalPE();
    data.resize(kMinimumHeadersSize + 2);
    auto* section = firstSection(data);
    section->VirtualAddress = 0x1FFF;
    section->PointerToRawData = kMinimumHeadersSize;
    section->SizeOfRawData = 2;

    PELoader loader;
    return !loader.loadFromMemory(data.data(), data.size());
}

static bool test_rejects_empty_file() {
    const std::string path = temporaryPath("-empty.bin");
    {
        std::ofstream file(path, std::ios::binary);
        if (!file)
            return false;
    }

    PELoader loader;
    const bool rejectedByLoad = !loader.load(path);
    PELoader dllLoader;
    const bool rejectedByLoadDLL = !dllLoader.loadDLL(path, "empty.dll");
    std::remove(path.c_str());
    return rejectedByLoad && rejectedByLoadDLL;
}

static bool test_rejects_oversized_file() {
    const std::string path = temporaryPath("-oversized.bin");
    {
        std::ofstream file(path, std::ios::binary);
        if (!file)
            return false;
        file.seekp(static_cast<std::streamoff>(kMaxPEFileSize - 1));
        file.put('\0');
        if (!file)
            return false;
    }

    PELoader loader;
    const bool rejectedByLoad = !loader.load(path);
    PELoader dllLoader;
    const bool rejectedByLoadDLL = !dllLoader.loadDLL(path, "oversized.dll");
    std::remove(path.c_str());
    return rejectedByLoad && rejectedByLoadDLL;
}

} // namespace

int main() {
    printf("=== PE Loader Bounds Tests ===\n\n");

    TEST(accepts_minimal_pe);
    TEST(rejects_optional_header_past_file);
    TEST(rejects_truncated_section_table);
    TEST(rejects_raw_section_past_file);
    TEST(rejects_virtual_section_past_image);
    TEST(rejects_empty_file);
    TEST(rejects_oversized_file);

    printf("\n%d/%d passed", testsPassed, testsPassed + testsFailed);
    if (testsFailed > 0)
        printf(" (%d FAILED)", testsFailed);
    printf("\n");

    return testsFailed > 0 ? 1 : 0;
}
