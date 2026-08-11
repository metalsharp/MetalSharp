#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <metalsharp/PEHeader.h>
#include <metalsharp/PELoader.h>
#include <vector>

using namespace metalsharp;

namespace {

constexpr size_t kPeHeaderOffset = 0x80;
constexpr size_t kRelocationOffset = 0x200;
constexpr size_t kRelocationTargetOffset = 0x1A0;
constexpr size_t kImageSize = 0x1000;
constexpr size_t kHeaderSize = 0x400;
constexpr uint64_t kImageBase = 0x140000000ULL;
constexpr uint64_t kRelocationValue = 0x1122334455667788ULL;

std::vector<uint8_t> makeImage(uint32_t relocationRVA = kRelocationOffset, uint32_t relocationSize = 12) {
    std::vector<uint8_t> image(kHeaderSize, 0);

    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(image.data());
    dos->e_magic = IMAGE_DOS_SIGNATURE;
    dos->e_lfanew = kPeHeaderOffset;

    auto* signature = reinterpret_cast<uint32_t*>(image.data() + kPeHeaderOffset);
    *signature = IMAGE_PE_SIGNATURE;

    auto* fileHeader = reinterpret_cast<IMAGE_FILE_HEADER*>(signature + 1);
    fileHeader->Machine = IMAGE_FILE_MACHINE_AMD64;
    fileHeader->SizeOfOptionalHeader = sizeof(IMAGE_OPTIONAL_HEADER64);

    auto* optionalHeader = reinterpret_cast<IMAGE_OPTIONAL_HEADER64*>(fileHeader + 1);
    optionalHeader->Magic = IMAGE_OPTIONAL_MAGIC_PE32PLUS;
    optionalHeader->ImageBase = kImageBase;
    optionalHeader->SectionAlignment = 0x1000;
    optionalHeader->FileAlignment = 0x200;
    optionalHeader->SizeOfImage = kImageSize;
    optionalHeader->SizeOfHeaders = kHeaderSize;
    optionalHeader->NumberOfRvaAndSizes = 16;
    optionalHeader->DataDirectory[DIRECTORY_BASERELOC].VirtualAddress = relocationRVA;
    optionalHeader->DataDirectory[DIRECTORY_BASERELOC].Size = relocationSize;

    auto* target = reinterpret_cast<uint64_t*>(image.data() + kRelocationTargetOffset);
    *target = kRelocationValue;

    return image;
}

void writeRelocationBlock(std::vector<uint8_t>& image, uint32_t pageRVA, uint16_t entry, uint32_t blockSize = 12) {
    auto* block = reinterpret_cast<IMAGE_BASE_RELOCATION*>(image.data() + kRelocationOffset);
    block->VirtualAddress = pageRVA;
    block->SizeOfBlock = blockSize;

    auto* entries = reinterpret_cast<uint16_t*>(block + 1);
    entries[0] = entry;
    entries[1] = static_cast<uint16_t>(IMAGE_REL_BASED_ABSOLUTE << 12);
}

bool rejectsMalformedImage(std::vector<uint8_t> image, const char* name) {
    PELoader loader;
    bool loaded = loader.loadFromMemory(image.data(), image.size());
    printf("  %-48s %s\n", name, loaded ? "FAIL" : "PASS");
    return !loaded;
}

} // namespace

int main() {
    int failures = 0;

    {
        auto image = makeImage();
        writeRelocationBlock(image, 0, static_cast<uint16_t>((IMAGE_REL_BASED_DIR64 << 12) | kRelocationTargetOffset));

        PELoader loader;
        bool loaded = loader.loadFromMemory(image.data(), image.size());
        bool valid = loaded && loader.getBase();
        if (valid) {
            auto* relocated = reinterpret_cast<const uint64_t*>(loader.getBase() + kRelocationTargetOffset);
            uint64_t delta = reinterpret_cast<uint64_t>(loader.getBase()) - kImageBase;
            valid = *relocated == kRelocationValue + delta;
        }
        printf("  %-48s %s\n", "valid DIR64 relocation is applied", valid ? "PASS" : "FAIL");
        failures += valid ? 0 : 1;
    }

    {
        auto image = makeImage(kRelocationOffset, 8);
        writeRelocationBlock(image, 0, 0, 4);
        failures += rejectsMalformedImage(image, "SizeOfBlock smaller than relocation header") ? 0 : 1;
    }

    {
        auto image = makeImage(kRelocationOffset, 12);
        writeRelocationBlock(image, 0, 0, 16);
        failures += rejectsMalformedImage(image, "SizeOfBlock exceeds relocation directory") ? 0 : 1;
    }

    {
        auto image = makeImage();
        writeRelocationBlock(image, 0x1000, static_cast<uint16_t>(IMAGE_REL_BASED_DIR64 << 12));
        failures += rejectsMalformedImage(image, "DIR64 target extends past image mapping") ? 0 : 1;
    }

    {
        auto image = makeImage();
        writeRelocationBlock(image, UINT32_MAX, static_cast<uint16_t>((IMAGE_REL_BASED_DIR64 << 12) | 0xFFF));
        failures += rejectsMalformedImage(image, "DIR64 target RVA uses 64-bit arithmetic") ? 0 : 1;
    }

    {
        auto image = makeImage(0x0FFF, 8);
        failures += rejectsMalformedImage(image, "relocation directory extends past image mapping") ? 0 : 1;
    }

    {
        auto image = makeImage(UINT32_MAX - 0x0F, 0x20);
        failures += rejectsMalformedImage(image, "relocation directory RVA arithmetic overflows") ? 0 : 1;
    }

    printf("\n%d relocation-bound checks failed\n", failures);
    return failures == 0 ? 0 : 1;
}
