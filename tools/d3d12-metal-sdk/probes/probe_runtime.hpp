#pragma once

// DXMT's D3D12 PE is built as a Wine builtin.  Wine can therefore resolve a
// stale builtin with the same module name even when a probe's application
// directory contains a freshly staged copy.  The isolated runner supplies a
// unique alias for the selected PE; redirect only the probe-side LoadLibraryA
// request for d3d12.dll to that alias.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstring>

// The toolchain may predefine NOMINMAX before this forced include. Windows
// has already consumed it above; leave the name available for probe sources
// that define it themselves before their own Windows include.
#ifdef NOMINMAX
#undef NOMINMAX
#endif

static inline HMODULE dxmt_probe_load_library_a(LPCSTR name) {
  using LoadLibraryAFunction = HMODULE(WINAPI *)(LPCSTR);
  static LoadLibraryAFunction original = nullptr;
  if (!original) {
    HMODULE kernel32 = GetModuleHandleA("kernel32.dll");
    FARPROC procedure = GetProcAddress(kernel32, "LoadLibraryA");
    static_assert(sizeof(original) == sizeof(procedure),
                  "function pointer size mismatch");
    std::memcpy(&original, &procedure, sizeof(original));
  }
  if (!original)
    return nullptr;

  const char *alias = nullptr;
  char alias_storage[128] = {};
  const char *alias_variable = nullptr;
  if (name) {
    if (std::strcmp(name, "d3d12.dll") == 0)
      alias_variable = "DXMT_PROBE_D3D12_DLL";
    else if (std::strcmp(name, "d3d11.dll") == 0)
      alias_variable = "DXMT_PROBE_D3D11_DLL";
    else if (std::strcmp(name, "d3d10core.dll") == 0)
      alias_variable = "DXMT_PROBE_D3D10CORE_DLL";
    else if (std::strcmp(name, "dxgi.dll") == 0)
      alias_variable = "DXMT_PROBE_DXGI_DLL";
  }
  if (alias_variable) {
    DWORD length = GetEnvironmentVariableA(alias_variable, alias_storage,
                                           sizeof(alias_storage));
    if (length > 0 && length < sizeof(alias_storage))
      alias = alias_storage;
  }
  return original(alias ? alias : name);
}

#define LoadLibraryA dxmt_probe_load_library_a
