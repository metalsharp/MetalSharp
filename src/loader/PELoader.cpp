/// @file PELoader.cpp
/// @brief Windows PE executable loader and import resolver.
///
/// PE Loading Pipeline
/// ===================
///
/// The loader processes Windows executables in 8 ordered phases:
///
///   1. parsePE()      — Validate DOS magic (MZ→0x5A4D), PE signature (PE→0x4550),
///                        machine type (AMD64 only), and extract optional header fields
///                        (ImageBase, SectionAlignment, FileAlignment, SizeOfImage).
///
///   2. mapSections()  — Allocate virtual memory via mmap(MAP_JIT on macOS for W^X
///                        compliance). Copy PE headers and each section's raw data from
///                        file offsets to virtual addresses. Sections are zero-padded to
///                        SectionAlignment granularity. The delta (actual_base - ImageBase)
///                        is computed for relocation fixups.
///
///   3. processRelocations() — Walk IMAGE_BASE_RELOCATION blocks. For each DIR64 entry
///                        (type 10), patch the 64-bit value at (pageRVA + offset) by
///                        adding the delta. ABSOLUTE entries (type 0) are skipped.
///
///   4. initCFG()      — Control Flow Guard bypass: patches GuardCFCheckFP and
///                        GuardCFDispatchFP in the load config directory to point at a
///                        dynamically-allocated "return TRUE" stub (mov eax,1; ret).
///                        This allows CFG-instrumented games to run without a real CFG
///                        bitmap.
///
///   5. resolveImports() — Walk IMAGE_IMPORT_DESCRIPTOR array. For each imported DLL:
///                        a) Check registered shims first (case-insensitive)
///                        b) Check already-loaded PE DLLs
///                        c) Attempt to load the DLL from search paths
///                        Each IAT slot is patched with the resolved function pointer.
///                        Both name-based and ordinal-based imports are handled.
///
///   6. resolveDelayImports() — Same logic as above but for IMAGE_DELAY_IMPORT_DESCRIPTOR.
///                        Delay-loaded DLLs are resolved eagerly at load time rather than
///                        on first call, since we can't hook the delay-load thunk.
///
///   7. applySectionProtections() — Set mprotect() on each section based on its
///                        Characteristics flags (EXECUTE/READ/WRITE). A minimum of
///                        READ|WRITE is enforced to avoid crashes from read-only data.
///
///   8. processTLS()   — Walk the TLS callback array from IMAGE_TLS_DIRECTORY64.
///                        Each callback is invoked with (moduleBase, DLL_PROCESS_ATTACH, nullptr).
///                        TLS callbacks run before DllMain for DLL dependencies.
///
/// Import Resolution Strategy
/// ==========================
///
///   resolveImport(dllName, funcName, ordinal) tries in order:
///     1. Registered ShimLibrary (m_shims) — MetalSharp's D3D/DXGI/audio/input interceptors
///     2. Loaded PE DLLs (m_loadedDLLs) — recursively loaded Windows DLLs
///     3. loadDependency() — search m_searchPaths for the DLL file
///
///   Export forwarding is handled: if an export RVA falls within the export directory,
///   it's a forward string like "NTDLL.RtlAllocateHeap" which is resolved recursively.
///
/// DLL Loading
/// ===========
///
///   loadDLL() performs the same pipeline (parse→map→reloc→imports→delays→protect→TLS)
///   for each dependency, then calls DllMain(moduleBase, DLL_PROCESS_ATTACH, nullptr)
///   if the DLL has the IMAGE_FILE_DLL characteristic.
///
/// Memory Management
/// =================
///
///   All PE images are mmap'd with MAP_PRIVATE|MAP_ANONYMOUS|MAP_JIT.
///   Destruction unmap()s all loaded modules. The singleton (s_instance) pointer
///   is set in the constructor and cleared in the destructor.
#include <cstring>
#include <fstream>
#include <limits>
#include <metalsharp/Logger.h>
#include <metalsharp/PEHeader.h>
#include <metalsharp/PELoader.h>
#include <sys/mman.h>

namespace metalsharp {
namespace {

/// Bounds-checked view into a mapped PE image.  Returns a pointer into the
/// image when [rva, rva + len) lies fully inside module.size, otherwise
/// nullptr.  Every descriptor/thunk/name access goes through this so a
/// malformed or truncated image cannot drive reads or writes outside the
/// mapping (metalsharp#413).
uint8_t* imagePtr(const LoadedModule& module, uint64_t rva, size_t len) {
    if (!module.base || len > module.size || rva > module.size - len)
        return nullptr;
    return module.base + rva;
}

template <typename T> T* imagePtr(const LoadedModule& module, uint64_t rva) {
    return reinterpret_cast<T*>(imagePtr(module, rva, sizeof(T)));
}

/// Copies a NUL-terminated string from inside the image into a fixed
/// buffer, never reading past the image.  Returns the number of bytes
/// copied (excluding the NUL), or 0 when rva is outside the image, the
/// string is empty, or outSize is zero.
size_t imageString(const LoadedModule& module, uint64_t rva, char* out, size_t outSize) {
    if (outSize == 0)
        return 0;
    out[0] = '\0';
    if (!module.base || rva >= module.size)
        return 0;
    size_t maxLen = module.size - (size_t)rva;
    if (maxLen > outSize - 1)
        maxLen = outSize - 1;
    const char* src = reinterpret_cast<const char*>(module.base + rva);
    size_t n = 0;
    while (n < maxLen && src[n] != '\0') {
        out[n] = src[n];
        n++;
    }
    out[n] = '\0';
    return n;
}

} // namespace

namespace {

constexpr std::streamoff kMaxPEFileSize = 512 * 1024 * 1024;

bool readPEFile(const std::string& path, std::vector<uint8_t>& data) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file.is_open())
        return false;

    const std::streamoff fileSize = file.tellg();
    if (fileSize <= 0 || fileSize >= kMaxPEFileSize)
        return false;
    if (!file.seekg(0, std::ios::beg))
        return false;

    data.resize(static_cast<size_t>(fileSize));
    return static_cast<bool>(
        file.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size())));
}

bool rangeWithin(size_t offset, size_t length, size_t total) {
    return offset <= total && length <= total - offset;
}

} // namespace

PELoader* PELoader::s_instance = nullptr;

PELoader::PELoader() {
    s_instance = this;
}
PELoader::~PELoader() {
    if (m_mainModule.base) {
        munmap(m_mainModule.base, m_mainModule.size);
    }
    for (auto& [name, mod] : m_loadedDLLs) {
        if (mod.base && mod.isPE) {
            munmap(mod.base, mod.size);
        }
    }
    s_instance = nullptr;
}

void PELoader::registerShim(const std::string& dllName, ShimLibrary&& shim) {
    std::string lower = dllName;
    for (auto& c : lower)
        c = tolower(c);
    m_shims[lower] = std::move(shim);
}

void* PELoader::resolveFunction(const std::string& dllName, const std::string& funcName) {
    return resolveImport(dllName, funcName, 0xFFFF);
}

bool PELoader::load(const std::string& path) {
    MS_INFO("PELoader: loading %s", path.c_str());

    m_mainModule.name = path;

    std::vector<uint8_t> data;
    if (!readPEFile(path, data)) {
        MS_INFO("PELoader: failed to read %s", path.c_str());
        return false;
    }
    const size_t fileSize = data.size();

    if (!parsePE(m_mainModule, data.data(), fileSize))
        return false;
    if (!mapSections(m_mainModule, data.data(), fileSize))
        return false;
    if (!processRelocations(m_mainModule))
        return false;
    if (!resolveImports(m_mainModule))
        return false;
    resolveDelayImports(m_mainModule);
    applySectionProtections(m_mainModule);
    processTLS(m_mainModule, DLL_PROCESS_ATTACH);

    MS_INFO("PELoader: loaded %s at %p, entry %p, size %u", path.c_str(), m_mainModule.base, m_mainModule.entryPoint,
            m_mainModule.size);
    return true;
}

bool PELoader::loadFromMemory(const uint8_t* data, size_t size) {
    if (!parsePE(m_mainModule, data, size))
        return false;
    if (!mapSections(m_mainModule, data, size))
        return false;
    if (!processRelocations(m_mainModule))
        return false;
    if (!resolveImports(m_mainModule))
        return false;
    resolveDelayImports(m_mainModule);
    applySectionProtections(m_mainModule);
    processTLS(m_mainModule, DLL_PROCESS_ATTACH);
    return true;
}

bool PELoader::parsePE(LoadedModule& module, const uint8_t* rawData, size_t rawSize) {
    if (!rawData || rawSize < sizeof(IMAGE_DOS_HEADER)) {
        MS_INFO("PELoader: file too small for DOS header");
        return false;
    }

    auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(rawData);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        MS_INFO("PELoader: not a valid PE file (bad DOS magic: 0x%04X)", dos->e_magic);
        return false;
    }

    const auto headerRangeWithin = [rawSize](size_t offset, size_t length) {
        return rangeWithin(offset, length, rawSize);
    };

    if (dos->e_lfanew < 0 ||
        !headerRangeWithin(static_cast<size_t>(dos->e_lfanew), sizeof(uint32_t) + sizeof(IMAGE_FILE_HEADER))) {
        MS_INFO("PELoader: invalid e_lfanew offset");
        return false;
    }

    auto* peSig = reinterpret_cast<const uint32_t*>(rawData + dos->e_lfanew);
    if (*peSig != IMAGE_PE_SIGNATURE) {
        MS_INFO("PELoader: bad PE signature: 0x%08X", *peSig);
        return false;
    }

    auto* fileHeader = reinterpret_cast<const IMAGE_FILE_HEADER*>(rawData + dos->e_lfanew + 4);

    if (fileHeader->Machine != IMAGE_FILE_MACHINE_AMD64) {
        MS_INFO("PELoader: unsupported machine type: 0x%04X (only AMD64 supported)", fileHeader->Machine);
        return false;
    }

    if (fileHeader->SizeOfOptionalHeader < sizeof(IMAGE_OPTIONAL_HEADER64)) {
        MS_INFO("PELoader: optional header too small");
        return false;
    }

    const size_t optionalHeaderOffset =
        static_cast<size_t>(dos->e_lfanew) + sizeof(uint32_t) + sizeof(IMAGE_FILE_HEADER);
    if (!headerRangeWithin(optionalHeaderOffset, fileHeader->SizeOfOptionalHeader)) {
        MS_INFO("PELoader: optional header extends past end of file");
        return false;
    }

    const size_t sectionTableOffset = optionalHeaderOffset + fileHeader->SizeOfOptionalHeader;
    const size_t sectionTableSize = static_cast<size_t>(fileHeader->NumberOfSections) * sizeof(IMAGE_SECTION_HEADER);
    if (!headerRangeWithin(sectionTableOffset, sectionTableSize)) {
        MS_INFO("PELoader: section table extends past end of file");
        return false;
    }

    auto* optHeader =
        reinterpret_cast<const IMAGE_OPTIONAL_HEADER64*>(rawData + dos->e_lfanew + 4 + sizeof(IMAGE_FILE_HEADER));

    if (optHeader->Magic != IMAGE_OPTIONAL_MAGIC_PE32PLUS) {
        MS_INFO("PELoader: not PE32+ (magic: 0x%04X)", optHeader->Magic);
        return false;
    }

    if (optHeader->SectionAlignment == 0 || optHeader->FileAlignment == 0) {
        MS_INFO("PELoader: invalid section or file alignment");
        return false;
    }

    const size_t sectionTableEnd = sectionTableOffset + sectionTableSize;
    if (optHeader->SizeOfHeaders < sectionTableEnd || optHeader->SizeOfHeaders > rawSize) {
        MS_INFO("PELoader: headers extend past declared or actual file bounds");
        return false;
    }

    m_imageBase = optHeader->ImageBase;
    m_sectionAlignment = optHeader->SectionAlignment;
    m_fileAlignment = optHeader->FileAlignment;

    module.size = optHeader->SizeOfImage;
    module.isPE = true;

    MS_INFO("PELoader: PE32+ image, %u sections, image base 0x%llX, size %u", fileHeader->NumberOfSections,
            (unsigned long long)optHeader->ImageBase, optHeader->SizeOfImage);

    return true;
}

bool PELoader::mapSections(LoadedModule& module, const uint8_t* rawData, size_t rawSize) {
    auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(rawData);
    auto* fileHeader = reinterpret_cast<const IMAGE_FILE_HEADER*>(rawData + dos->e_lfanew + 4);
    auto* optHeader =
        reinterpret_cast<const IMAGE_OPTIONAL_HEADER64*>(rawData + dos->e_lfanew + 4 + sizeof(IMAGE_FILE_HEADER));
    auto* sections = reinterpret_cast<const IMAGE_SECTION_HEADER*>(
        rawData + dos->e_lfanew + 4 + sizeof(IMAGE_FILE_HEADER) + fileHeader->SizeOfOptionalHeader);

    const uint64_t imageSize64 = alignUp(optHeader->SizeOfImage, 0x1000);
    if (imageSize64 == 0 || imageSize64 > std::numeric_limits<size_t>::max()) {
        MS_INFO("PELoader: invalid image size");
        return false;
    }
    const size_t imageSize = static_cast<size_t>(imageSize64);

#ifdef __APPLE__
    int mmapFlags = MAP_PRIVATE | MAP_ANONYMOUS | MAP_JIT;
#else
    int mmapFlags = MAP_PRIVATE | MAP_ANONYMOUS;
#endif

    uint8_t* mem =
        reinterpret_cast<uint8_t*>(mmap(nullptr, imageSize, PROT_READ | PROT_WRITE | PROT_EXEC, mmapFlags, -1, 0));

    if (mem == MAP_FAILED) {
        MS_INFO("PELoader: mmap failed for image (%zu bytes), errno=%d", imageSize, errno);
        return false;
    }

    memset(mem, 0, imageSize);

    const uint64_t headerSize64 = alignUp(optHeader->SizeOfHeaders, m_fileAlignment);
    if (headerSize64 > imageSize || headerSize64 > rawSize) {
        MS_INFO("PELoader: header copy exceeds image or file bounds");
        munmap(mem, imageSize);
        return false;
    }
    memcpy(mem, rawData, static_cast<size_t>(headerSize64));

    for (uint16_t i = 0; i < fileHeader->NumberOfSections; i++) {
        const auto& sec = sections[i];
        if (sec.SizeOfRawData == 0)
            continue;

        uint32_t dstOffset = sec.VirtualAddress;
        uint32_t srcOffset = sec.PointerToRawData;
        uint32_t copySize = sec.SizeOfRawData;

        if (static_cast<size_t>(srcOffset) > rawSize ||
            static_cast<size_t>(copySize) > rawSize - static_cast<size_t>(srcOffset)) {
            MS_INFO("PELoader: section raw data exceeds file bounds");
            munmap(mem, imageSize);
            return false;
        }
        if (static_cast<size_t>(dstOffset) > imageSize ||
            static_cast<size_t>(copySize) > imageSize - static_cast<size_t>(dstOffset)) {
            MS_INFO("PELoader: section virtual data exceeds image bounds");
            munmap(mem, imageSize);
            return false;
        }

        if (copySize > 0) {
            memcpy(mem + dstOffset, rawData + srcOffset, copySize);
        }

        const char* name = reinterpret_cast<const char*>(sec.Name);
        MS_INFO("PELoader: mapped section %.8s at RVA 0x%X (0x%X bytes, raw 0x%X)", name, sec.VirtualAddress,
                sec.VirtualSize, sec.SizeOfRawData);
    }

    module.base = mem;
    if (optHeader->AddressOfEntryPoint) {
        module.entryPoint = mem + optHeader->AddressOfEntryPoint;
    }

    m_delta = reinterpret_cast<uint64_t>(mem) - m_imageBase;

    return true;
}

bool PELoader::processRelocations(LoadedModule& module) {
    if (!module.base)
        return false;

    auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(module.base);
    auto* fileHeader = reinterpret_cast<const IMAGE_FILE_HEADER*>(module.base + dos->e_lfanew + 4);
    auto* optHeader =
        reinterpret_cast<const IMAGE_OPTIONAL_HEADER64*>(module.base + dos->e_lfanew + 4 + sizeof(IMAGE_FILE_HEADER));

    if (optHeader->DataDirectory[DIRECTORY_BASERELOC].Size == 0) {
        MS_INFO("PELoader: no relocations needed");
        return true;
    }

    uint32_t relocRVA = optHeader->DataDirectory[DIRECTORY_BASERELOC].VirtualAddress;
    uint32_t relocSize = optHeader->DataDirectory[DIRECTORY_BASERELOC].Size;

    auto* reloc = reinterpret_cast<IMAGE_BASE_RELOCATION*>(module.base + relocRVA);
    auto* relocEnd = reinterpret_cast<IMAGE_BASE_RELOCATION*>(module.base + relocRVA + relocSize);

    int relocCount = 0;

    while (reloc < relocEnd && reloc->SizeOfBlock > 0) {
        uint32_t pageRVA = reloc->VirtualAddress;
        uint32_t numEntries = (reloc->SizeOfBlock - sizeof(IMAGE_BASE_RELOCATION)) / sizeof(uint16_t);
        auto* entries = reinterpret_cast<uint16_t*>(reloc + 1);

        for (uint32_t i = 0; i < numEntries; i++) {
            uint16_t entry = entries[i];
            uint32_t type = entry >> 12;
            uint32_t offset = entry & 0xFFF;

            if (type == IMAGE_REL_BASED_DIR64) {
                uint64_t* target = reinterpret_cast<uint64_t*>(module.base + pageRVA + offset);
                *target += m_delta;
                relocCount++;
            } else if (type == IMAGE_REL_BASED_ABSOLUTE) {
                // skip
            } else {
                MS_INFO("PELoader: unsupported relocation type %u at 0x%X", type, pageRVA + offset);
            }
        }

        reloc = reinterpret_cast<IMAGE_BASE_RELOCATION*>(reinterpret_cast<uint8_t*>(reloc) + reloc->SizeOfBlock);
    }

    MS_INFO("PELoader: processed %d relocations (delta 0x%llX)", relocCount, (unsigned long long)m_delta);

    if (!initCFG(module))
        return false;

    return true;
}

bool PELoader::initCFG(LoadedModule& module) {
    auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(module.base);
    auto* optHeader =
        reinterpret_cast<const IMAGE_OPTIONAL_HEADER64*>(module.base + dos->e_lfanew + 4 + sizeof(IMAGE_FILE_HEADER));

    uint32_t lcRVA = optHeader->DataDirectory[DIRECTORY_LOAD_CONFIG].VirtualAddress;
    uint32_t lcSize = optHeader->DataDirectory[DIRECTORY_LOAD_CONFIG].Size;

    if (!lcRVA || lcSize < 128)
        return true;

    auto* lc = reinterpret_cast<uint8_t*>(module.base + lcRVA);

    uint64_t guardCFCheckFPRVA = *reinterpret_cast<uint64_t*>(lc + 112);
    uint64_t guardCFDispatchFPRVA = *reinterpret_cast<uint64_t*>(lc + 120);

    if (!s_cfgAllowFn) {
#ifdef __APPLE__
        s_cfgAllowFn =
            mmap(nullptr, 4096, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS | MAP_JIT, -1, 0);
#else
        s_cfgAllowFn = mmap(nullptr, 4096, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
#endif
        if (s_cfgAllowFn == MAP_FAILED) {
            s_cfgAllowFn = nullptr;
            return true;
        }
        uint8_t code[] = {0xB8, 0x01, 0x00, 0x00, 0x00, 0xC3};
        memcpy(s_cfgAllowFn, code, sizeof(code));
    }

    auto writeCFPtr = [&](uint64_t fieldRVA, const char* name) {
        if (!fieldRVA || fieldRVA >= module.size)
            return;
        uint64_t* ptr = reinterpret_cast<uint64_t*>(module.base + fieldRVA);
        MS_INFO("PELoader: CFG %s at RVA 0x%llX set to allow-all stub", name, (unsigned long long)fieldRVA);
        *ptr = reinterpret_cast<uint64_t>(s_cfgAllowFn);
    };

    if (guardCFCheckFPRVA)
        writeCFPtr(guardCFCheckFPRVA, "GuardCFCheckFP");
    if (guardCFDispatchFPRVA)
        writeCFPtr(guardCFDispatchFPRVA, "GuardCFDispatchFP");

    return true;
}

void* PELoader::s_cfgAllowFn = nullptr;

bool PELoader::resolveImports(LoadedModule& module) {
    if (!module.base)
        return false;

    auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(module.base);
    auto* optHeader =
        reinterpret_cast<const IMAGE_OPTIONAL_HEADER64*>(module.base + dos->e_lfanew + 4 + sizeof(IMAGE_FILE_HEADER));

    if (optHeader->DataDirectory[DIRECTORY_IMPORT].Size == 0) {
        MS_INFO("PELoader: no imports");
        return true;
    }

    uint32_t importRVA = optHeader->DataDirectory[DIRECTORY_IMPORT].VirtualAddress;
    uint32_t importSize = optHeader->DataDirectory[DIRECTORY_IMPORT].Size;

    /* Bound the descriptor walk by the import directory size instead of
     * trusting a zero terminator that malformed images may never provide. */
    size_t descCount = importSize / sizeof(IMAGE_IMPORT_DESCRIPTOR);
    for (size_t d = 0; d < descCount; d++) {
        auto* importDesc =
            imagePtr<IMAGE_IMPORT_DESCRIPTOR>(module, (uint64_t)importRVA + d * sizeof(IMAGE_IMPORT_DESCRIPTOR));
        if (!importDesc)
            break;
        if (importDesc->Name == 0 || importDesc->FirstThunk == 0)
            break;

        char dllBuf[128];
        if (imageString(module, importDesc->Name, dllBuf, sizeof(dllBuf)) == 0) {
            MS_INFO("PELoader: skipping import descriptor %zu with invalid Name RVA 0x%X", d, importDesc->Name);
            continue;
        }
        std::string dllName(dllBuf);
        MS_INFO("PELoader: resolving imports from %s", dllName.c_str());

        uint64_t thunkRVA = importDesc->OriginalFirstThunk ? importDesc->OriginalFirstThunk : importDesc->FirstThunk;
        uint64_t iatRVA = importDesc->FirstThunk;

        int resolved = 0;
        int failed = 0;

        /* Walk the INT and patch the IAT only while both stay inside the
         * image; a malformed entry or a missing terminator ends the walk
         * instead of reading or writing past the mapping. */
        while (true) {
            uint64_t* thunk = imagePtr<uint64_t>(module, thunkRVA);
            if (!thunk)
                break;
            uint64_t entry = *thunk;
            if (entry == 0)
                break;
            uint64_t* iat = imagePtr<uint64_t>(module, iatRVA);
            if (!iat)
                break;

            void* funcPtr = nullptr;
            std::string missingName;
            uint16_t missingOrdinal = 0;
            bool isOrdinal = false;

            if (entry & (1ULL << 63)) {
                uint16_t ordinal = static_cast<uint16_t>(entry & 0xFFFF);
                missingOrdinal = ordinal;
                isOrdinal = true;
                funcPtr = resolveImport(dllName, "", ordinal);
            } else {
                uint64_t nameRVA = entry & 0x7FFFFFFF;
                if (imagePtr(module, nameRVA, offsetof(IMAGE_IMPORT_BY_NAME, Name)) != nullptr) {
                    char funcBuf[256];
                    if (imageString(module, nameRVA + offsetof(IMAGE_IMPORT_BY_NAME, Name), funcBuf, sizeof(funcBuf)) !=
                        0) {
                        missingName = funcBuf;
                        funcPtr = resolveImport(dllName, missingName, 0xFFFF);
                    }
                }
            }

            if (funcPtr) {
                *iat = reinterpret_cast<uint64_t>(funcPtr);
                resolved++;
            } else {
                if (!isOrdinal) {
                    MS_INFO("PELoader:   MISSING %s!%s", dllName.c_str(), missingName.c_str());
                } else {
                    MS_INFO("PELoader:   MISSING %s!ordinal_%u", dllName.c_str(), missingOrdinal);
                }
                failed++;
                *iat = 0;
            }

            thunkRVA += sizeof(uint64_t);
            iatRVA += sizeof(uint64_t);
        }

        MS_INFO("PELoader: %s — %d resolved, %d failed", dllName.c_str(), resolved, failed);
    }

    return true;
}

void* PELoader::resolveImport(const std::string& dllName, const std::string& funcName, uint16_t ordinal) {
    std::string lower = dllName;
    for (auto& c : lower)
        c = tolower(c);

    auto shimIt = m_shims.find(lower);
    if (shimIt != m_shims.end()) {
        if (!funcName.empty()) {
            auto it = shimIt->second.functions.find(funcName);
            if (it != shimIt->second.functions.end()) {
                return it->second();
            }
        }
        if (ordinal != 0xFFFF) {
            auto it = shimIt->second.ordinals.find(ordinal);
            if (it != shimIt->second.ordinals.end()) {
                return it->second();
            }
        }
    }

    auto dllIt = m_loadedDLLs.find(lower);
    if (dllIt != m_loadedDLLs.end()) {
        return getExportAddress(dllIt->second, funcName, ordinal);
    }

    LoadedModule depModule;
    if (loadDependency(lower, depModule)) {
        m_loadedDLLs[lower] = std::move(depModule);
        auto& stored = m_loadedDLLs[lower];
        return getExportAddress(stored, funcName, ordinal);
    }

    return nullptr;
}

void* PELoader::getExportAddress(LoadedModule& module, const std::string& funcName, uint16_t ordinal) {
    if (!module.base)
        return nullptr;

    auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(module.base);
    auto* optHeader =
        reinterpret_cast<const IMAGE_OPTIONAL_HEADER64*>(module.base + dos->e_lfanew + 4 + sizeof(IMAGE_FILE_HEADER));

    if (optHeader->DataDirectory[DIRECTORY_EXPORT].Size == 0)
        return nullptr;

    uint32_t exportRVA = optHeader->DataDirectory[DIRECTORY_EXPORT].VirtualAddress;
    uint32_t exportSize = optHeader->DataDirectory[DIRECTORY_EXPORT].Size;
    auto* exportDir = imagePtr<IMAGE_EXPORT_DIRECTORY>(module, exportRVA);
    if (!exportDir)
        return nullptr;

    /* The three parallel export arrays must fit inside the image before any
     * element is dereferenced; otherwise every lookup below would read an
     * unchecked RVA. */
    uint64_t namesRVA = exportDir->AddressOfNames;
    uint64_t funcsRVA = exportDir->AddressOfFunctions;
    uint64_t ordsRVA = exportDir->AddressOfNameOrdinals;
    uint64_t nameBytes = (uint64_t)exportDir->NumberOfNames * sizeof(uint32_t);
    uint64_t funcBytes = (uint64_t)exportDir->NumberOfFunctions * sizeof(uint32_t);
    uint64_t ordBytes = (uint64_t)exportDir->NumberOfNames * sizeof(uint16_t);
    if (!imagePtr(module, namesRVA, nameBytes) || !imagePtr(module, funcsRVA, funcBytes) ||
        !imagePtr(module, ordsRVA, ordBytes)) {
        return nullptr;
    }

    auto* names = reinterpret_cast<uint32_t*>(module.base + namesRVA);
    auto* functions = reinterpret_cast<uint32_t*>(module.base + funcsRVA);
    auto* ordinals = reinterpret_cast<uint16_t*>(module.base + ordsRVA);

    uint32_t funcRVA = 0;

    if (!funcName.empty()) {
        for (uint32_t i = 0; i < exportDir->NumberOfNames; i++) {
            char nameBuf[256];
            if (imageString(module, names[i], nameBuf, sizeof(nameBuf)) == 0)
                continue;
            if (strcmp(nameBuf, funcName.c_str()) == 0) {
                uint16_t idx = ordinals[i];
                if (idx < exportDir->NumberOfFunctions) {
                    funcRVA = functions[idx];
                }
                break;
            }
        }
    }

    if (funcRVA == 0 && ordinal != 0xFFFF && ordinal >= exportDir->Base) {
        uint32_t idx = ordinal - exportDir->Base;
        if (idx < exportDir->NumberOfFunctions) {
            funcRVA = functions[idx];
        }
    }

    if (funcRVA == 0)
        return nullptr;

    if (funcRVA >= exportRVA && funcRVA < (uint64_t)exportRVA + exportSize) {
        char fwdBuf[256];
        if (imageString(module, funcRVA, fwdBuf, sizeof(fwdBuf)) == 0)
            return nullptr;
        return resolveForwardedExport(fwdBuf);
    }

    if (funcRVA >= module.size)
        return nullptr;

    return module.base + funcRVA;
}

void* PELoader::resolveForwardedExport(const char* forwardString) {
    if (!forwardString)
        return nullptr;

    std::string fwd(forwardString);
    auto dot = fwd.find('.');
    if (dot == std::string::npos)
        return nullptr;

    std::string dllName = fwd.substr(0, dot) + ".dll";
    std::string funcName = fwd.substr(dot + 1);

    for (auto& c : dllName)
        c = tolower(c);

    auto it = m_loadedDLLs.find(dllName);
    if (it != m_loadedDLLs.end()) {
        return getExportAddress(it->second, funcName);
    }

    LoadedModule depModule;
    if (loadDependency(dllName, depModule)) {
        m_loadedDLLs[dllName] = std::move(depModule);
        return getExportAddress(m_loadedDLLs[dllName], funcName);
    }

    return resolveImport(dllName, funcName, 0xFFFF);
}

void PELoader::processTLS(LoadedModule& module, uint32_t reason) {
    if (!module.base)
        return;

    auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(module.base);
    auto* optHeader =
        reinterpret_cast<const IMAGE_OPTIONAL_HEADER64*>(module.base + dos->e_lfanew + 4 + sizeof(IMAGE_FILE_HEADER));

    if (optHeader->DataDirectory[DIRECTORY_TLS].Size == 0)
        return;

    uint32_t tlsRVA = optHeader->DataDirectory[DIRECTORY_TLS].VirtualAddress;
    auto* tlsDir = reinterpret_cast<IMAGE_TLS_DIRECTORY64*>(module.base + tlsRVA);

    uint64_t callbacksVA = tlsDir->AddressOfCallBacks;
    if (callbacksVA == 0)
        return;

    uint64_t callbacksRVA;
    if (callbacksVA >= m_imageBase && callbacksVA < m_imageBase + module.size) {
        callbacksRVA = callbacksVA - m_imageBase;
    } else {
        callbacksRVA = callbacksVA - reinterpret_cast<uint64_t>(module.base);
        if (callbacksRVA >= module.size)
            return;
    }

    auto** callbacks = reinterpret_cast<void**>(module.base + callbacksRVA);

    MS_INFO("PELoader: processing TLS callbacks for %s", module.name.c_str());

    typedef void (*TLSCallback)(void*, uint32_t, void*);
    for (int i = 0; callbacks[i] != nullptr; i++) {
        auto cb = reinterpret_cast<TLSCallback>(callbacks[i]);
        MS_INFO("PELoader: calling TLS callback %d at %p", i, (void*)cb);
        cb(reinterpret_cast<void*>(module.base), reason, nullptr);
    }
}

bool PELoader::resolveDelayImports(LoadedModule& module) {
    if (!module.base)
        return true;

    auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(module.base);
    auto* optHeader =
        reinterpret_cast<const IMAGE_OPTIONAL_HEADER64*>(module.base + dos->e_lfanew + 4 + sizeof(IMAGE_FILE_HEADER));

    if (optHeader->NumberOfRvaAndSizes <= 13)
        return true;
    if (optHeader->DataDirectory[DIRECTORY_DELAY_IMPORT].Size == 0)
        return true;

    uint32_t delayRVA = optHeader->DataDirectory[DIRECTORY_DELAY_IMPORT].VirtualAddress;
    uint32_t delaySize = optHeader->DataDirectory[DIRECTORY_DELAY_IMPORT].Size;

    size_t count = delaySize / sizeof(IMAGE_DELAY_IMPORT_DESCRIPTOR);
    for (size_t i = 0; i < count; i++) {
        auto* desc = imagePtr<IMAGE_DELAY_IMPORT_DESCRIPTOR>(module, (uint64_t)delayRVA +
                                                                         i * sizeof(IMAGE_DELAY_IMPORT_DESCRIPTOR));
        if (!desc)
            break;
        if (desc->rvaDLLName == 0)
            break;

        char dllBuf[128];
        if (imageString(module, desc->rvaDLLName, dllBuf, sizeof(dllBuf)) == 0)
            break;
        std::string dllNameStr(dllBuf);
        MS_INFO("PELoader: resolving delay-load import: %s", dllNameStr.c_str());

        /* Walk the delay INT and patch the IAT only while both stay inside
         * the image; a malformed entry or a missing terminator ends the
         * walk instead of reading or writing past the mapping. */
        uint64_t intRVA = desc->rvaINT;
        uint64_t iatRVA = desc->rvaIAT;
        while (true) {
            uint64_t* entryPtr = imagePtr<uint64_t>(module, intRVA);
            if (!entryPtr)
                break;
            uint64_t entry = *entryPtr;
            if (entry == 0)
                break;
            uint64_t* iatSlot = imagePtr<uint64_t>(module, iatRVA);
            if (!iatSlot)
                break;

            void* funcPtr = nullptr;
            if (entry & (1ULL << 63)) {
                uint16_t ordinal = static_cast<uint16_t>(entry & 0xFFFF);
                funcPtr = resolveImport(dllNameStr, "", ordinal);
            } else {
                uint64_t nameRVA = entry & 0x7FFFFFFF;
                if (imagePtr(module, nameRVA, offsetof(IMAGE_IMPORT_BY_NAME, Name)) != nullptr) {
                    char funcBuf[256];
                    if (imageString(module, nameRVA + offsetof(IMAGE_IMPORT_BY_NAME, Name), funcBuf, sizeof(funcBuf)) !=
                        0) {
                        funcPtr = resolveImport(dllNameStr, funcBuf, 0xFFFF);
                    }
                }
            }

            *iatSlot = reinterpret_cast<uint64_t>(funcPtr);
            intRVA += sizeof(uint64_t);
            iatRVA += sizeof(uint64_t);
        }

        HMODULE hMod = loadLibrary(dllNameStr);
        if (hMod && desc->rvaHmod) {
            if (auto* hmodSlot = imagePtr<HMODULE>(module, desc->rvaHmod)) {
                *hmodSlot = hMod;
            }
        }
    }

    return true;
}

void PELoader::applySectionProtections(LoadedModule& module) {
    if (!module.base)
        return;

    auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(module.base);
    auto* fileHeader = reinterpret_cast<const IMAGE_FILE_HEADER*>(module.base + dos->e_lfanew + 4);
    auto* sections = reinterpret_cast<const IMAGE_SECTION_HEADER*>(
        module.base + dos->e_lfanew + 4 + sizeof(IMAGE_FILE_HEADER) + fileHeader->SizeOfOptionalHeader);

    for (uint16_t i = 0; i < fileHeader->NumberOfSections; i++) {
        auto& sec = sections[i];
        if (sec.VirtualSize == 0 || sec.VirtualAddress == 0)
            continue;

        uint32_t pageRVA = sec.VirtualAddress;
        uint32_t pageSize = alignUp(sec.VirtualSize, 4096);
        uint32_t chars = sec.Characteristics;

        int prot = PROT_READ;
        if (chars & IMAGE_SCN_MEM_EXECUTE)
            prot |= PROT_EXEC;
        if (chars & IMAGE_SCN_MEM_WRITE)
            prot |= PROT_WRITE;

        if ((chars & IMAGE_SCN_CNT_CODE) && !(chars & IMAGE_SCN_MEM_WRITE)) {
            prot |= PROT_EXEC;
        }

        if (prot == PROT_READ) {
            prot = PROT_READ | PROT_WRITE;
        }

        void* addr = module.base + pageRVA;
        mprotect(addr, pageSize, prot);
    }
}

void* PELoader::lookupFunctionEntry(uint64_t controlPc, uint64_t* outImageBase) {
    struct RuntimeFunction {
        uint32_t BeginAddress;
        uint32_t EndAddress;
        uint32_t UnwindData;
    };

    for (auto& [name, mod] : m_loadedDLLs) {
        if (!mod.base || !mod.isPE)
            continue;
        auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(mod.base);
        auto* opt =
            reinterpret_cast<const IMAGE_OPTIONAL_HEADER64*>(mod.base + dos->e_lfanew + 4 + sizeof(IMAGE_FILE_HEADER));
        if (opt->DataDirectory[DIRECTORY_EXCEPTION].Size == 0)
            continue;

        uint64_t modBase = reinterpret_cast<uint64_t>(mod.base);
        uint64_t modEnd = modBase + mod.size;
        if (controlPc < modBase || controlPc >= modEnd)
            continue;

        uint32_t exceptRva = opt->DataDirectory[DIRECTORY_EXCEPTION].VirtualAddress;
        uint32_t exceptSize = opt->DataDirectory[DIRECTORY_EXCEPTION].Size;
        auto* funcs = reinterpret_cast<const RuntimeFunction*>(mod.base + exceptRva);
        size_t count = exceptSize / sizeof(RuntimeFunction);

        uint32_t rva = static_cast<uint32_t>(controlPc - modBase);
        for (size_t i = 0; i < count; i++) {
            if (rva >= funcs[i].BeginAddress && rva < funcs[i].EndAddress) {
                if (outImageBase)
                    *outImageBase = modBase;
                MS_INFO("PELoader: lookupFunctionEntry(0x%llX) found in %s at RVA 0x%X", (unsigned long long)controlPc,
                        name.c_str(), rva);
                return const_cast<RuntimeFunction*>(&funcs[i]);
            }
        }
    }

    if (m_mainModule.base && m_mainModule.isPE) {
        auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(m_mainModule.base);
        auto* opt = reinterpret_cast<const IMAGE_OPTIONAL_HEADER64*>(m_mainModule.base + dos->e_lfanew + 4 +
                                                                     sizeof(IMAGE_FILE_HEADER));
        if (opt->DataDirectory[DIRECTORY_EXCEPTION].Size > 0) {
            uint64_t modBase = reinterpret_cast<uint64_t>(m_mainModule.base);
            uint32_t exceptRva = opt->DataDirectory[DIRECTORY_EXCEPTION].VirtualAddress;
            uint32_t exceptSize = opt->DataDirectory[DIRECTORY_EXCEPTION].Size;
            auto* funcs = reinterpret_cast<const RuntimeFunction*>(m_mainModule.base + exceptRva);
            size_t count = exceptSize / sizeof(RuntimeFunction);

            uint32_t rva = static_cast<uint32_t>(controlPc - modBase);
            for (size_t i = 0; i < count; i++) {
                if (rva >= funcs[i].BeginAddress && rva < funcs[i].EndAddress) {
                    if (outImageBase)
                        *outImageBase = modBase;
                    MS_INFO("PELoader: lookupFunctionEntry(0x%llX) found in main module at RVA 0x%X",
                            (unsigned long long)controlPc, rva);
                    return const_cast<RuntimeFunction*>(&funcs[i]);
                }
            }
        }
    }

    if (outImageBase)
        *outImageBase = 0;
    return nullptr;
}

bool PELoader::loadDependency(const std::string& dllName, LoadedModule& outModule) {
    std::vector<std::string> paths = m_searchPaths;
    if (!m_mainModule.name.empty()) {
        auto lastSlash = m_mainModule.name.find_last_of("/\\");
        if (lastSlash != std::string::npos) {
            paths.push_back(m_mainModule.name.substr(0, lastSlash));
        }
    }

    for (const auto& dir : paths) {
        std::string path = dir + "/" + dllName;
        std::ifstream f(path);
        if (f.is_open()) {
            f.close();
            return loadDLL(path, dllName);
        }
    }

    return false;
}

LoadedModule* PELoader::getModule(const std::string& name) {
    std::string lower = name;
    for (auto& c : lower)
        c = tolower(c);

    if (m_loadedDLLs.count(lower))
        return &m_loadedDLLs[lower];
    return nullptr;
}

PELoader* PELoader::instance() {
    return s_instance;
}

void PELoader::addSearchPath(const std::string& path) {
    m_searchPaths.push_back(path);
}

HMODULE PELoader::loadLibrary(const std::string& dllName) {
    std::string lower = dllName;
    for (auto& c : lower)
        c = tolower(c);

    auto existing = m_loadedDLLs.find(lower);
    if (existing != m_loadedDLLs.end()) {
        return reinterpret_cast<HMODULE>(existing->second.base);
    }

    std::string lowerWithDll = lower;
    if (lower.size() < 4 || lower.substr(lower.size() - 4) != ".dll") {
        lowerWithDll = lower + ".dll";
    }

    for (auto& name : {lower, lowerWithDll}) {
        auto shimIt = m_shims.find(name);
        if (shimIt != m_shims.end()) {
            HMODULE fake = reinterpret_cast<HMODULE>(0x2);
            m_moduleHandles[fake] = name;
            return fake;
        }
    }

    for (const auto& dir : m_searchPaths) {
        for (auto& name : {lower, lowerWithDll}) {
            std::string path = dir + "/" + name;
            std::ifstream f(path);
            if (f.is_open()) {
                f.close();
                if (loadDLL(path, name)) {
                    return reinterpret_cast<HMODULE>(m_loadedDLLs[name].base);
                }
            }
        }
    }

    MS_INFO("PELoader: LoadLibrary(\"%s\") — not found, returning shim handle", dllName.c_str());
    HMODULE fake = reinterpret_cast<HMODULE>(0x2);
    m_moduleHandles[fake] = lowerWithDll;
    return fake;
}

void* PELoader::getProcAddress(HMODULE hModule, const std::string& funcName) {
    auto it = m_moduleHandles.find(hModule);
    if (it != m_moduleHandles.end()) {
        std::string lower = it->second;
        auto shimIt = m_shims.find(lower);
        if (shimIt != m_shims.end()) {
            auto fit = shimIt->second.functions.find(funcName);
            if (fit != shimIt->second.functions.end()) {
                MS_INFO("PELoader: GetProcAddress(%s, %s) -> %p", lower.c_str(), funcName.c_str(), fit->second());
                return fit->second();
            }
            MS_INFO("PELoader: GetProcAddress(%s, %s) -> NOT FOUND in shim", lower.c_str(), funcName.c_str());
        }
    }

    for (auto& [name, mod] : m_loadedDLLs) {
        if (reinterpret_cast<HMODULE>(mod.base) == hModule) {
            void* addr = getExportAddress(mod, funcName);
            MS_INFO("PELoader: GetProcAddress(PE:%s, %s) -> %p", name.c_str(), funcName.c_str(), addr);
            return addr;
        }
    }

    MS_INFO("PELoader: GetProcAddress(%p, %s) -> null", hModule, funcName.c_str());
    return nullptr;
}

bool PELoader::loadDLL(const std::string& path, const std::string& dllName) {
    MS_INFO("PELoader: loading DLL %s from %s", dllName.c_str(), path.c_str());

    std::vector<uint8_t> data;
    if (!readPEFile(path, data))
        return false;
    const size_t fileSize = data.size();

    LoadedModule mod;
    mod.name = dllName;
    if (!parsePE(mod, data.data(), fileSize))
        return false;
    if (!mapSections(mod, data.data(), fileSize))
        return false;
    if (!processRelocations(mod))
        return false;
    if (!resolveImports(mod))
        return false;
    resolveDelayImports(mod);

    m_loadedDLLs[dllName] = std::move(mod);
    auto& stored = m_loadedDLLs[dllName];

    applySectionProtections(stored);
    processTLS(stored, DLL_PROCESS_ATTACH);

    if (stored.entryPoint) {
        auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(stored.base);
        auto* fileHdr = reinterpret_cast<const IMAGE_FILE_HEADER*>(stored.base + dos->e_lfanew + 4);
        if (fileHdr->Characteristics & IMAGE_FILE_DLL) {
            typedef int (*DllMainProc)(void*, unsigned long, void*);
            auto dllMain = reinterpret_cast<DllMainProc>(stored.entryPoint);
            dllMain(reinterpret_cast<void*>(stored.base), 1, nullptr);
            MS_INFO("PELoader: DllMain(%s) called", dllName.c_str());
        }
    }

    return true;
}

} // namespace metalsharp
