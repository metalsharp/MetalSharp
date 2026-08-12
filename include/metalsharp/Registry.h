/// @file Registry.h
/// @brief Windows registry simulation backed by an in-memory key/value store.
///
/// Implements HKEY-based registry operations (open, create, query, set, delete) using
/// a nested unordered_map structure persisted to disk as a simple key-value file. Seeds
/// common registry paths that games query at startup (Steam install paths, DirectX version,
/// display driver info). Thread-safe via mutex. Games that read HKLM\SOFTWARE\... during
/// initialization are serviced entirely by this shim.

#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "Win32Error.h"

namespace metalsharp {
namespace win32 {

typedef int32_t LONG;
typedef uint32_t DWORD;
typedef uint8_t BYTE;
typedef void* HKEY;

struct RegistryValue {
    DWORD type;
    std::vector<BYTE> data;
};

class Registry {
  public:
    static Registry& instance();

    void init(const std::string& prefix);

    LONG openKey(HKEY hKey, const std::string& subKey, HKEY* result);
    LONG openKeyEx(HKEY hKey, const std::string& subKey, DWORD ulOptions, DWORD samDesired, HKEY* result);
    LONG createKeyEx(HKEY hKey, const std::string& subKey, DWORD reserved, char* lpClass, DWORD dwOptions,
                     DWORD samDesired, void* lpSecurityAttributes, HKEY* phkResult, void* lpdwDisposition);
    LONG closeKey(HKEY hKey);
    LONG queryValue(HKEY hKey, const std::string& valueName, DWORD* lpType, BYTE* lpData, DWORD* lpcbData);
    LONG setValue(HKEY hKey, const std::string& valueName, DWORD dwType, const BYTE* lpData, DWORD cbData);
    LONG deleteValue(HKEY hKey, const std::string& valueName);
    LONG deleteKey(HKEY hKey, const std::string& subKey);

    void saveToFile(const std::string& path);
    void loadFromFile(const std::string& path);

  private:
    Registry();

    std::string normalizePath(HKEY hKey, const std::string& subKey);
    std::string keyToString(HKEY hKey);

    void seedSteam(const std::string& prefix);

    std::unordered_map<std::string, std::unordered_map<std::string, RegistryValue>> m_store;
    std::unordered_map<HKEY, std::string> m_openKeys;
    uintptr_t m_nextKey = 0xA000;
    std::mutex m_mutex;
};

} // namespace win32
} // namespace metalsharp
