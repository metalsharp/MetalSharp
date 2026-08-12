#include <cstdint>
#include <cstdio>
#include <cstring>
#include <metalsharp/ExtraShims.h>
#include <metalsharp/PEHeader.h>
#include <metalsharp/PELoader.h>
#include <metalsharp/Win32Types.h>
#include <sys/mman.h>

using namespace metalsharp;
using namespace metalsharp::win32;

namespace {

constexpr uint32_t kImageSize = 0x1000;
constexpr uint32_t kResourceRVA = 0x200;
constexpr uint32_t kResourceSize = 0x80;
constexpr uint32_t kTypeID = 10;
constexpr uint32_t kNameID = 20;
constexpr uint32_t kTypeDirectoryOffset = 0x20;
constexpr uint32_t kNameDirectoryOffset = 0x40;
constexpr uint32_t kDataEntryOffset = 0x60;
constexpr uint32_t kResourceDataRVA = 0x500;

using FindResourceA = void*(MSABI*)(metalsharp::win32::HMODULE, const char*, const char*);
using LoadResource = void*(MSABI*)(metalsharp::win32::HMODULE, void*);
using LockResource = void*(MSABI*)(void*);
using SizeofResource = metalsharp::win32::DWORD(MSABI*)(metalsharp::win32::HMODULE, void*);

struct ResourceApi {
    FindResourceA find;
    LoadResource load;
    LockResource lock;
    SizeofResource size;
};

struct TestImage {
    PELoader loader;
    uint8_t* base = nullptr;

    TestImage() {
        base = static_cast<uint8_t*>(
            mmap(nullptr, kImageSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
        if (base == MAP_FAILED) {
            base = nullptr;
            return;
        }

        memset(base, 0, kImageSize);
        auto* module = loader.getMainModule();
        module->base = base;
        module->size = kImageSize;
        module->isPE = true;
        initializeHeaders();
    }

    ~TestImage() {
        if (base) {
            loader.getMainModule()->base = nullptr;
            munmap(base, kImageSize);
        }
    }

    bool valid() const { return base != nullptr; }

    void initializeHeaders() {
        auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
        dos->e_magic = IMAGE_DOS_SIGNATURE;
        dos->e_lfanew = 0x80;

        auto* peSignature = reinterpret_cast<uint32_t*>(base + dos->e_lfanew);
        *peSignature = IMAGE_PE_SIGNATURE;

        auto* fileHeader = reinterpret_cast<IMAGE_FILE_HEADER*>(peSignature + 1);
        fileHeader->Machine = IMAGE_FILE_MACHINE_AMD64;
        fileHeader->SizeOfOptionalHeader = sizeof(IMAGE_OPTIONAL_HEADER64);

        auto* optionalHeader = reinterpret_cast<IMAGE_OPTIONAL_HEADER64*>(fileHeader + 1);
        optionalHeader->Magic = IMAGE_OPTIONAL_MAGIC_PE32PLUS;
        optionalHeader->SizeOfImage = kImageSize;
        optionalHeader->NumberOfRvaAndSizes = 16;
        optionalHeader->DataDirectory[DIRECTORY_RESOURCE].VirtualAddress = kResourceRVA;
        optionalHeader->DataDirectory[DIRECTORY_RESOURCE].Size = kResourceSize;
    }

    IMAGE_OPTIONAL_HEADER64* optionalHeader() {
        auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
        auto* fileHeader = reinterpret_cast<IMAGE_FILE_HEADER*>(base + dos->e_lfanew + sizeof(uint32_t));
        return reinterpret_cast<IMAGE_OPTIONAL_HEADER64*>(fileHeader + 1);
    }

    void clearResource() { memset(base + kResourceRVA, 0, kResourceSize); }

    void buildValidResource() {
        initializeHeaders();
        clearResource();

        auto* root = reinterpret_cast<IMAGE_RESOURCE_DIRECTORY*>(base + kResourceRVA);
        root->NumberOfIdEntries = 1;
        auto* rootEntry = reinterpret_cast<IMAGE_RESOURCE_DIRECTORY_ENTRY*>(root + 1);
        rootEntry->Name = kTypeID;
        rootEntry->OffsetToData = 0x80000000u | kTypeDirectoryOffset;

        auto* typeDirectory = reinterpret_cast<IMAGE_RESOURCE_DIRECTORY*>(base + kResourceRVA + kTypeDirectoryOffset);
        typeDirectory->NumberOfIdEntries = 1;
        auto* typeEntry = reinterpret_cast<IMAGE_RESOURCE_DIRECTORY_ENTRY*>(typeDirectory + 1);
        typeEntry->Name = kNameID;
        typeEntry->OffsetToData = 0x80000000u | kNameDirectoryOffset;

        auto* nameDirectory = reinterpret_cast<IMAGE_RESOURCE_DIRECTORY*>(base + kResourceRVA + kNameDirectoryOffset);
        nameDirectory->NumberOfIdEntries = 1;
        auto* nameEntry = reinterpret_cast<IMAGE_RESOURCE_DIRECTORY_ENTRY*>(nameDirectory + 1);
        nameEntry->OffsetToData = kDataEntryOffset;

        auto* dataEntry = reinterpret_cast<IMAGE_RESOURCE_DATA_ENTRY*>(base + kResourceRVA + kDataEntryOffset);
        dataEntry->OffsetToData = kResourceDataRVA;
        dataEntry->Size = 4;
        memcpy(base + kResourceDataRVA, "rsrc", 4);
    }

    IMAGE_RESOURCE_DIRECTORY_ENTRY* rootEntry() {
        auto* root = reinterpret_cast<IMAGE_RESOURCE_DIRECTORY*>(base + kResourceRVA);
        return reinterpret_cast<IMAGE_RESOURCE_DIRECTORY_ENTRY*>(root + 1);
    }

    IMAGE_RESOURCE_DATA_ENTRY* dataEntry() {
        return reinterpret_cast<IMAGE_RESOURCE_DATA_ENTRY*>(base + kResourceRVA + kDataEntryOffset);
    }
};

const char* resourceID(uint32_t id) {
    return reinterpret_cast<const char*>(static_cast<uintptr_t>(id));
}

ResourceApi createResourceApi() {
    ShimLibrary kernel32;
    addMissingKernel32(kernel32);
    return {
        reinterpret_cast<FindResourceA>(kernel32.functions.at("FindResourceA")()),
        reinterpret_cast<LoadResource>(kernel32.functions.at("LoadResource")()),
        reinterpret_cast<LockResource>(kernel32.functions.at("LockResource")()),
        reinterpret_cast<SizeofResource>(kernel32.functions.at("SizeofResource")()),
    };
}

bool testValidResource(const ResourceApi& api, TestImage& image) {
    image.buildValidResource();
    void* resource = api.find(nullptr, resourceID(kNameID), resourceID(kTypeID));
    if (resource != image.base + kResourceRVA + kDataEntryOffset)
        return false;

    void* loaded = api.load(nullptr, resource);
    if (loaded != resource)
        return false;
    if (api.size(nullptr, loaded) != 4)
        return false;

    void* data = api.lock(loaded);
    return data == image.base + kResourceDataRVA && memcmp(data, "rsrc", 4) == 0;
}

bool testDirectoryEntryCountIsBounded(const ResourceApi& api, TestImage& image) {
    image.clearResource();
    auto* root = reinterpret_cast<IMAGE_RESOURCE_DIRECTORY*>(image.base + kResourceRVA);
    root->NumberOfNamedEntries = UINT16_MAX;
    root->NumberOfIdEntries = UINT16_MAX;
    return api.find(nullptr, resourceID(kNameID), resourceID(kTypeID)) == nullptr;
}

bool testDirectoryOffsetIsBounded(const ResourceApi& api, TestImage& image) {
    image.buildValidResource();
    image.rootEntry()->OffsetToData = 0x80000000u | kResourceSize;
    return api.find(nullptr, resourceID(kNameID), resourceID(kTypeID)) == nullptr;
}

bool testDataRvaAndSizeAreBounded(const ResourceApi& api, TestImage& image) {
    image.buildValidResource();
    image.dataEntry()->OffsetToData = kImageSize - 1;
    image.dataEntry()->Size = 2;
    if (api.find(nullptr, resourceID(kNameID), resourceID(kTypeID)) != nullptr)
        return false;

    image.buildValidResource();
    image.dataEntry()->Size = kImageSize;
    void* resource = api.find(nullptr, resourceID(kNameID), resourceID(kTypeID));
    return resource == nullptr && api.lock(image.dataEntry()) == nullptr && api.size(nullptr, image.dataEntry()) == 0;
}

bool testImageAndHeaderBoundsAreChecked(const ResourceApi& api, TestImage& image) {
    image.buildValidResource();
    image.optionalHeader()->DataDirectory[DIRECTORY_RESOURCE].VirtualAddress = kImageSize - 4;
    image.optionalHeader()->DataDirectory[DIRECTORY_RESOURCE].Size = 16;
    if (api.find(nullptr, resourceID(kNameID), resourceID(kTypeID)) != nullptr)
        return false;

    image.buildValidResource();
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(image.base);
    dos->e_lfanew = static_cast<int32_t>(kImageSize - sizeof(uint32_t) + 1);
    if (api.find(nullptr, resourceID(kNameID), resourceID(kTypeID)) != nullptr)
        return false;

    image.buildValidResource();
    image.optionalHeader()->NumberOfRvaAndSizes = DIRECTORY_RESOURCE;
    if (api.find(nullptr, resourceID(kNameID), resourceID(kTypeID)) != nullptr)
        return false;

    image.buildValidResource();
    auto* invalidHandle = image.base + kImageSize - sizeof(IMAGE_RESOURCE_DATA_ENTRY) + 1;
    return api.load(nullptr, invalidHandle) == nullptr && api.lock(invalidHandle) == nullptr &&
           api.size(nullptr, invalidHandle) == 0;
}

} // namespace

int main() {
    TestImage image;
    if (!image.valid()) {
        fprintf(stderr, "resource_bounds: failed to allocate test image\n");
        return 1;
    }

    ResourceApi api = createResourceApi();
    struct TestCase {
        const char* name;
        bool (*run)(const ResourceApi&, TestImage&);
    };
    const TestCase tests[] = {
        {"valid resource tree", testValidResource},
        {"directory entry count", testDirectoryEntryCountIsBounded},
        {"directory offset", testDirectoryOffsetIsBounded},
        {"resource data RVA and size", testDataRvaAndSizeAreBounded},
        {"image, directory, and handle bounds", testImageAndHeaderBoundsAreChecked},
    };

    int failed = 0;
    for (const auto& test : tests) {
        bool passed = test.run(api, image);
        printf("  [%s] %s\n", passed ? "PASS" : "FAIL", test.name);
        failed += passed ? 0 : 1;
    }

    printf("resource_bounds: %zu tests, %d failed\n", sizeof(tests) / sizeof(tests[0]), failed);
    return failed == 0 ? 0 : 1;
}
