/// @file test_pe_loader.cpp
/// @brief Regression tests for bounds-checked PE import/export resolution
///        (metalsharp/MetalSharp#413).
///
/// The loader previously walked import descriptors and IAT thunks without
/// bounds and dereferenced caller-controlled RVAs, so a malformed image
/// could drive reads/writes far outside the mapped PE.  These tests feed
/// the real PELoader deliberately malformed images (non-terminated
/// descriptor tables, out-of-image RVAs, out-of-range export ordinals) and
/// verify it skips them gracefully while still resolving well-formed
/// imports and exports.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <unistd.h>
#include <vector>

#include <metalsharp/PEHeader.h>
#include <metalsharp/PELoader.h>

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

/// Builds a minimal PE32+ image in memory.  Every directory and its data
/// lives inside the header region (RVA == file offset), which mapSections
/// copies verbatim into the mapped image, so the crafted structures are
/// reachable without declaring any sections.
class TestPEBuilder {
  public:
    static constexpr uint32_t kHeaderSize = 0x1000;
    static constexpr uint32_t kImageSize = 0x4000;

    TestPEBuilder() : m_data(kHeaderSize, 0) {}

    /// Appends a NUL-terminated string; returns its RVA.
    uint32_t putString(const char* s) {
        uint32_t rva = rawAlloc(strlen(s) + 1);
        memcpy(m_data.data() + rva, s, strlen(s) + 1);
        return rva;
    }

    /// Appends an IMAGE_IMPORT_BY_NAME (2-byte hint + name); returns the
    /// RVA a thunk entry must point at.
    uint32_t putImportByName(const char* name) {
        uint32_t rva = rawAlloc(2 + strlen(name) + 1);
        uint8_t* p = m_data.data() + rva;
        p[0] = 0;
        p[1] = 0;
        memcpy(p + 2, name, strlen(name) + 1);
        return rva;
    }

    /// Appends an 8-byte-aligned array of 64-bit thunk values; returns RVA.
    uint32_t putThunks(std::initializer_list<uint64_t> values) {
        uint32_t rva = alignedAlloc(values.size() * sizeof(uint64_t));
        uint64_t* p = reinterpret_cast<uint64_t*>(m_data.data() + rva);
        size_t i = 0;
        for (uint64_t v : values)
            p[i++] = v;
        return rva;
    }

    /// Appends an import descriptor; returns its RVA.  Descriptors are
    /// placed back-to-back so tables and their terminator stay contiguous.
    uint32_t putImportDescriptor(uint32_t originalFirstThunk, uint32_t name, uint32_t firstThunk) {
        IMAGE_IMPORT_DESCRIPTOR d{};
        d.OriginalFirstThunk = originalFirstThunk;
        d.Name = name;
        d.FirstThunk = firstThunk;
        uint32_t rva = rawAlloc(sizeof(d));
        memcpy(m_data.data() + rva, &d, sizeof(d));
        return rva;
    }

    /// Appends a delay-import descriptor; returns its RVA.
    uint32_t putDelayDescriptor(uint32_t dllName, uint32_t hmod, uint32_t iat, uint32_t intRVA) {
        IMAGE_DELAY_IMPORT_DESCRIPTOR d{};
        d.rvaDLLName = dllName;
        d.rvaHmod = hmod;
        d.rvaIAT = iat;
        d.rvaINT = intRVA;
        uint32_t rva = rawAlloc(sizeof(d));
        memcpy(m_data.data() + rva, &d, sizeof(d));
        return rva;
    }

    void setImportDirectory(uint32_t rva, uint32_t size) {
        m_importRVA = rva;
        m_importSize = size;
    }
    void setDelayDirectory(uint32_t rva, uint32_t size) {
        m_delayRVA = rva;
        m_delaySize = size;
    }
    void setExportDirectory(uint32_t rva, uint32_t size) {
        m_exportRVA = rva;
        m_exportSize = size;
    }

    /// Direct access for tests that place directories at fixed RVAs.
    uint8_t* mutableBytes() { return m_data.data(); }

    std::vector<uint8_t> finish() {
        IMAGE_DOS_HEADER dos{};
        dos.e_magic = IMAGE_DOS_SIGNATURE;
        dos.e_lfanew = 0x40;
        memcpy(m_data.data(), &dos, sizeof(dos));

        uint32_t signature = IMAGE_PE_SIGNATURE;
        memcpy(m_data.data() + 0x40, &signature, sizeof(signature));

        IMAGE_FILE_HEADER fileHeader{};
        fileHeader.Machine = IMAGE_FILE_MACHINE_AMD64;
        fileHeader.NumberOfSections = 0;
        fileHeader.SizeOfOptionalHeader = sizeof(IMAGE_OPTIONAL_HEADER64);
        fileHeader.Characteristics = 0x0002;
        memcpy(m_data.data() + 0x44, &fileHeader, sizeof(fileHeader));

        IMAGE_OPTIONAL_HEADER64 opt{};
        opt.Magic = IMAGE_OPTIONAL_MAGIC_PE32PLUS;
        opt.ImageBase = 0x140000000ULL;
        opt.SectionAlignment = 0x1000;
        opt.FileAlignment = 0x200;
        opt.SizeOfImage = kImageSize;
        opt.SizeOfHeaders = kHeaderSize;
        opt.NumberOfRvaAndSizes = 16;
        if (m_importSize != 0) {
            opt.DataDirectory[DIRECTORY_IMPORT].VirtualAddress = m_importRVA;
            opt.DataDirectory[DIRECTORY_IMPORT].Size = m_importSize;
        }
        if (m_delaySize != 0) {
            opt.DataDirectory[DIRECTORY_DELAY_IMPORT].VirtualAddress = m_delayRVA;
            opt.DataDirectory[DIRECTORY_DELAY_IMPORT].Size = m_delaySize;
        }
        if (m_exportSize != 0) {
            opt.DataDirectory[DIRECTORY_EXPORT].VirtualAddress = m_exportRVA;
            opt.DataDirectory[DIRECTORY_EXPORT].Size = m_exportSize;
        }
        memcpy(m_data.data() + 0x58, &opt, sizeof(opt));

        return m_data;
    }

  private:
    uint32_t rawAlloc(size_t len) {
        uint32_t rva = static_cast<uint32_t>(m_cursor);
        m_cursor += len;
        if (m_cursor > kHeaderSize) {
            fprintf(stderr, "TestPEBuilder: header region exhausted\n");
            exit(2);
        }
        return rva;
    }

    uint32_t alignedAlloc(size_t len) {
        m_cursor = (m_cursor + 7) & ~size_t(7);
        return rawAlloc(len);
    }

    std::vector<uint8_t> m_data;
    size_t m_cursor = 0x200;
    uint32_t m_importRVA = 0;
    uint32_t m_importSize = 0;
    uint32_t m_delayRVA = 0;
    uint32_t m_delaySize = 0;
    uint32_t m_exportRVA = 0;
    uint32_t m_exportSize = 0;
};

/// Reads back a patched IAT slot from the mapped image.
static uint64_t readIatSlot(PELoader& loader, uint32_t iatRVA, size_t index) {
    auto* slot = reinterpret_cast<uint64_t*>(loader.getMainModule()->base + iatRVA + index * sizeof(uint64_t));
    return *slot;
}

/// Registers a shim DLL with one function so well-formed imports can be
/// resolved against a known pointer.
static void registerShimFunc(PELoader& loader, const char* dll, const char* func, void* ptr) {
    ShimLibrary lib;
    lib.name = dll;
    lib.functions[func] = [ptr]() -> void* { return ptr; };
    loader.registerShim(dll, std::move(lib));
}

/// Writes a crafted DLL to a unique temp file; returns its directory.
static std::string writeTempDll(const std::string& name, const std::vector<uint8_t>& data) {
    std::string dir = "/tmp";
    std::string path = dir + "/" + name;
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    out.close();
    return dir;
}

static bool test_valid_imports_resolve() {
    PELoader loader;
    registerShimFunc(loader, "shimk32.dll", "ShimFunc", reinterpret_cast<void*>(0x1234));

    TestPEBuilder pe;
    uint32_t shimName = pe.putString("shimk32.dll");
    uint32_t missingDll = pe.putString("missing.dll");
    uint32_t shimFunc = pe.putImportByName("ShimFunc");
    uint32_t noSuchFunc = pe.putImportByName("NoSuchFunc");
    uint32_t intA = pe.putThunks({shimFunc, 0});
    uint32_t iatA = pe.putThunks({0, 0});
    uint32_t intB = pe.putThunks({noSuchFunc, 0});
    uint32_t iatB = pe.putThunks({0, 0});
    uint32_t desc0 = pe.putImportDescriptor(intA, shimName, iatA);
    uint32_t desc1 = pe.putImportDescriptor(intB, missingDll, iatB);
    uint32_t desc2 = pe.putImportDescriptor(0, 0, 0);
    pe.setImportDirectory(desc0, 3 * sizeof(IMAGE_IMPORT_DESCRIPTOR));
    (void)desc1;
    (void)desc2;
    auto image = pe.finish();

    if (!loader.loadFromMemory(image.data(), image.size()))
        return false;
    if (readIatSlot(loader, iatA, 0) != 0x1234)
        return false;
    if (readIatSlot(loader, iatB, 0) != 0)
        return false;
    return true;
}

static bool test_import_descriptor_table_bounded() {
    PELoader loader;
    registerShimFunc(loader, "shimk32.dll", "ShimFunc", reinterpret_cast<void*>(0x1234));

    TestPEBuilder pe;
    uint32_t shimName = pe.putString("shimk32.dll");
    uint32_t shimFunc = pe.putImportByName("ShimFunc");
    uint32_t intA = pe.putThunks({shimFunc, 0});
    uint32_t iatA = pe.putThunks({0, 0});
    uint32_t desc0 = pe.putImportDescriptor(intA, shimName, iatA);
    // Second descriptor sits outside the declared directory size and is not
    // zero-terminated: the walk must stop at the directory boundary instead
    // of dereferencing its wild FirstThunk RVA.
    pe.putImportDescriptor(0, shimName, 0xFFFFFF00u);
    pe.setImportDirectory(desc0, sizeof(IMAGE_IMPORT_DESCRIPTOR));
    auto image = pe.finish();

    if (!loader.loadFromMemory(image.data(), image.size()))
        return false;
    return readIatSlot(loader, iatA, 0) == 0x1234;
}

static bool test_import_name_rva_out_of_image() {
    PELoader loader;

    TestPEBuilder pe;
    uint32_t iatA = pe.putThunks({0, 0});
    uint32_t desc0 = pe.putImportDescriptor(0, 0x7FFFFFF0u, iatA);
    uint32_t desc1 = pe.putImportDescriptor(0, 0, 0);
    pe.setImportDirectory(desc0, 2 * sizeof(IMAGE_IMPORT_DESCRIPTOR));
    (void)desc1;
    auto image = pe.finish();

    if (!loader.loadFromMemory(image.data(), image.size()))
        return false;
    return readIatSlot(loader, iatA, 0) == 0;
}

static bool test_import_thunk_rva_out_of_image() {
    PELoader loader;

    TestPEBuilder pe;
    uint32_t shimName = pe.putString("shimk32.dll");
    uint32_t iatA = pe.putThunks({0, 0});
    uint32_t desc0 = pe.putImportDescriptor(0x7FFFFFF0u, shimName, iatA);
    uint32_t desc1 = pe.putImportDescriptor(0, 0, 0);
    pe.setImportDirectory(desc0, 2 * sizeof(IMAGE_IMPORT_DESCRIPTOR));
    (void)desc1;
    auto image = pe.finish();

    if (!loader.loadFromMemory(image.data(), image.size()))
        return false;
    return readIatSlot(loader, iatA, 0) == 0;
}

static bool test_import_by_name_rva_out_of_image() {
    PELoader loader;

    TestPEBuilder pe;
    uint32_t shimName = pe.putString("shimk32.dll");
    uint32_t intA = pe.putThunks({0x7FFFFFF0u, 0});
    uint32_t iatA = pe.putThunks({0, 0});
    uint32_t desc0 = pe.putImportDescriptor(intA, shimName, iatA);
    uint32_t desc1 = pe.putImportDescriptor(0, 0, 0);
    pe.setImportDirectory(desc0, 2 * sizeof(IMAGE_IMPORT_DESCRIPTOR));
    (void)desc1;
    auto image = pe.finish();

    if (!loader.loadFromMemory(image.data(), image.size()))
        return false;
    return readIatSlot(loader, iatA, 0) == 0;
}

static bool test_delay_import_wild_ints() {
    PELoader loader;
    registerShimFunc(loader, "shimk32.dll", "ShimFunc", reinterpret_cast<void*>(0x1234));

    TestPEBuilder pe;
    uint32_t shimName = pe.putString("shimk32.dll");
    uint32_t iatA = pe.putThunks({0, 0});
    uint32_t desc0 = pe.putDelayDescriptor(shimName, 0x7FFFFFF0u, iatA, 0x7FFFFFF0u);
    pe.setDelayDirectory(desc0, sizeof(IMAGE_DELAY_IMPORT_DESCRIPTOR));
    auto image = pe.finish();

    if (!loader.loadFromMemory(image.data(), image.size()))
        return false;
    return readIatSlot(loader, iatA, 0) == 0;
}

static bool test_delay_import_by_name_rva_out_of_image() {
    PELoader loader;

    TestPEBuilder pe;
    uint32_t shimName = pe.putString("shimk32.dll");
    uint32_t intA = pe.putThunks({0x7FFFFFF0u, 0});
    uint32_t iatA = pe.putThunks({0, 0});
    uint32_t desc0 = pe.putDelayDescriptor(shimName, 0, iatA, intA);
    pe.setDelayDirectory(desc0, sizeof(IMAGE_DELAY_IMPORT_DESCRIPTOR));
    auto image = pe.finish();

    if (!loader.loadFromMemory(image.data(), image.size()))
        return false;
    return readIatSlot(loader, iatA, 0) == 0;
}

/// Builds a DLL exporting "Good" (ordinal 0) and "FuncA" whose name-ordinal
/// pair points past NumberOfFunctions.
static std::vector<uint8_t> buildExportDll(bool wildNamesArray) {
    TestPEBuilder pe;
    uint32_t dirRVA = 0x300;
    uint32_t funcRVA = 0x200;

    // Place the directory and its three parallel arrays at fixed RVAs.
    IMAGE_EXPORT_DIRECTORY dir{};
    dir.Base = 1;
    dir.NumberOfFunctions = 1;
    dir.NumberOfNames = 1;
    dir.AddressOfFunctions = dirRVA + 40;
    dir.AddressOfNameOrdinals = dirRVA + 44;
    dir.AddressOfNames = wildNamesArray ? 0x7FFFFFF0u : dirRVA + 48;
    memcpy(pe.mutableBytes() + dirRVA, &dir, sizeof(dir));

    uint32_t* funcs = reinterpret_cast<uint32_t*>(pe.mutableBytes() + dirRVA + 40);
    funcs[0] = funcRVA;
    uint16_t* ordinals = reinterpret_cast<uint16_t*>(pe.mutableBytes() + dirRVA + 44);
    ordinals[0] = 0xFFFF; // out of range for NumberOfFunctions == 1
    uint32_t* names = reinterpret_cast<uint32_t*>(pe.mutableBytes() + dirRVA + 48);
    names[0] = pe.putString("FuncA");

    pe.setExportDirectory(dirRVA, 0x80);
    return pe.finish();
}

static bool test_export_ordinal_out_of_range() {
    std::string dllName = "ms413_" + std::to_string(getpid()) + ".dll";
    std::string dir = writeTempDll(dllName, buildExportDll(false));

    PELoader loader;
    loader.addSearchPath(dir);
    if (!loader.loadDLL(dir + "/" + dllName, dllName))
        return false;
    auto* module = loader.getModule(dllName);
    if (!module || !module->base)
        return false;

    // The name lookup must not index functions[] with the out-of-range
    // ordinal; it should fail cleanly instead of reading past the array.
    void* addr = loader.getProcAddress(reinterpret_cast<HMODULE>(module->base), "FuncA");
    if (addr != nullptr)
        return false;

    std::remove((dir + "/" + dllName).c_str());
    return true;
}

static bool test_export_arrays_out_of_image() {
    std::string dllName = "ms413_" + std::to_string(getpid()) + "_b.dll";
    std::string dir = writeTempDll(dllName, buildExportDll(true));

    PELoader loader;
    loader.addSearchPath(dir);
    if (!loader.loadDLL(dir + "/" + dllName, dllName))
        return false;
    auto* module = loader.getModule(dllName);
    if (!module || !module->base)
        return false;

    void* addr = loader.getProcAddress(reinterpret_cast<HMODULE>(module->base), "FuncA");
    if (addr != nullptr)
        return false;

    std::remove((dir + "/" + dllName).c_str());
    return true;
}

int main() {
    printf("\n--- PELoader import/export bounds regression ---\n");

    TEST(valid_imports_resolve);
    TEST(import_descriptor_table_bounded);
    TEST(import_name_rva_out_of_image);
    TEST(import_thunk_rva_out_of_image);
    TEST(import_by_name_rva_out_of_image);
    TEST(delay_import_wild_ints);
    TEST(delay_import_by_name_rva_out_of_image);
    TEST(export_ordinal_out_of_range);
    TEST(export_arrays_out_of_image);

    printf("\npe_loader: %d passed, %d failed\n", testsPassed, testsFailed);
    return testsFailed == 0 ? 0 : 1;
}
