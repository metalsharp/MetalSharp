/// @file PELoader.h
/// @brief PE32+ loader — loads Windows executables and DLLs on macOS.
///
/// PELoader is the heart of MetalSharp's native execution path. It parses
/// PE32/PE32+ executables, maps sections into memory, processes relocations,
/// resolves imports (both regular and delayed), handles TLS callbacks, and
/// dispatches DllMain entry points.
///
/// Loading pipeline (in order):
///   1. parsePE()              — Validate DOS/PE headers, extract optional header
///   2. mapSections()          — Map raw sections into aligned virtual memory (mmap MAP_JIT)
///   3. processRelocations()   — Apply base relocation fixups (IMAGE_REL_BASED_DIR64)
///   4. initCFG()              — Bypass Control Flow Guard (patch to "return TRUE" stub)
///   5. resolveImports()       — Walk import descriptors, resolve via shims or native DLLs
///   6. resolveDelayImports()  — Same for delay-loaded DLLs (resolved eagerly)
///   7. applySectionProtections() — Set mprotect based on section characteristics
///   8. processTLS()           — Run TLS callbacks for DLL_PROCESS_ATTACH
///
/// File-backed loads reject empty or 512 MiB-and-larger inputs. Header and
/// section-table extents must remain within the raw file before mapping.
///
/// Import resolution priority: registered shim → loaded PE DLL → load from search path.
/// Export forwarding is handled recursively.
/// Shims are registered via registerShim() and take priority over native loading.

#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace metalsharp {

typedef void* HMODULE;

struct LoadedModule {
    std::string name;
    uint8_t* base = nullptr;
    uint32_t size = 0;
    uint8_t* entryPoint = nullptr;
    bool isPE = false;

    uint8_t* rvaToPtr(uint32_t rva) const {
        if (!base || rva >= size)
            return nullptr;
        return base + rva;
    }
};

using ExportedFunction = std::function<void*()>;

struct ShimLibrary {
    std::string name;
    std::unordered_map<std::string, ExportedFunction> functions;
    std::unordered_map<uint16_t, ExportedFunction> ordinals;
};

class PELoader {
  public:
    PELoader();
    ~PELoader();

    bool load(const std::string& path);
    bool loadFromMemory(const uint8_t* data, size_t size);
    bool loadDLL(const std::string& path, const std::string& dllName);

    LoadedModule* getModule(const std::string& name);
    LoadedModule* getMainModule() { return &m_mainModule; }
    HMODULE loadLibrary(const std::string& dllName);
    void* getProcAddress(HMODULE hModule, const std::string& funcName);

    void registerShim(const std::string& dllName, ShimLibrary&& shim);
    void* resolveFunction(const std::string& dllName, const std::string& funcName);

    void* lookupFunctionEntry(uint64_t controlPc, uint64_t* outImageBase);

    uint8_t* getBase() const { return m_mainModule.base; }

    void addSearchPath(const std::string& path);

    static PELoader* instance();

  private:
    bool parsePE(LoadedModule& module, const uint8_t* rawData, size_t rawSize);
    bool mapSections(LoadedModule& module, const uint8_t* rawData, size_t rawSize);
    bool processRelocations(LoadedModule& module);
    bool resolveImports(LoadedModule& module);
    bool resolveDelayImports(LoadedModule& module);
    void processTLS(LoadedModule& module, uint32_t reason);
    void applySectionProtections(LoadedModule& module);
    bool loadDependency(const std::string& dllName, LoadedModule& outModule);
    bool initCFG(LoadedModule& module);

    void* resolveImport(const std::string& dllName, const std::string& funcName, uint16_t ordinal);
    void* getExportAddress(LoadedModule& module, const std::string& funcName, uint16_t ordinal = 0xFFFF);
    void* resolveForwardedExport(const char* forwardString);

    uint64_t alignUp(uint64_t value, uint64_t alignment) { return (value + alignment - 1) & ~(alignment - 1); }

    LoadedModule m_mainModule;
    std::unordered_map<std::string, LoadedModule> m_loadedDLLs;
    std::unordered_map<std::string, ShimLibrary> m_shims;
    std::vector<std::string> m_searchPaths;
    std::unordered_map<HMODULE, std::string> m_moduleHandles;

    uint64_t m_imageBase = 0;
    uint32_t m_sectionAlignment = 0;
    uint32_t m_fileAlignment = 0;
    uint64_t m_delta = 0;

    static PELoader* s_instance;
    static void* s_cfgAllowFn;
};

} // namespace metalsharp
