/// @file Win32Types.h
/// @brief Win32 ABI types and constants for the kernel32/ntdll shim layer.
///
/// Defines Win32 handle types, process/memory/file constants, and system
/// structures (MEMORY_BASIC_INFORMATION, CRITICAL_SECTION, SYSTEM_INFO, etc.)
/// used by the PE loader and Win32 shims. All types carry the `ms_abi`
/// calling-convention attribute so they can be called from PE (Windows x86-64)
/// code running under Wine. Lives in metalsharp::win32 namespace to avoid
/// colliding with real Win32 headers.

#pragma once

#include <cstddef>
#include <cstdint>

#define MSABI __attribute__((ms_abi))

namespace metalsharp {
namespace win32 {

typedef void* HANDLE;
typedef void* HMODULE;
typedef void* HWND;
typedef void* HINSTANCE;
typedef uint32_t DWORD;
typedef int32_t BOOL;
typedef int32_t LONG;
typedef uint16_t WORD;
typedef uint8_t BYTE;
typedef size_t SIZE_T;
typedef unsigned int UINT;
typedef long HRESULT;
typedef uint32_t ULONG;
typedef uintptr_t DWORD_PTR;
typedef int (*FARPROC)();

// Error values used by the kernel32 compatibility surface.
constexpr DWORD ERROR_INVALID_PARAMETER = 87;
constexpr DWORD ERROR_BUFFER_OVERFLOW = 111;
constexpr DWORD ERROR_ENVVAR_NOT_FOUND = 203;

const HANDLE INVALID_HANDLE_VALUE = reinterpret_cast<HANDLE>(static_cast<intptr_t>(-1));
constexpr DWORD DWORD_MAX_VAL = 0xFFFFFFFF;

struct MEMORY_BASIC_INFORMATION {
    void* BaseAddress;
    void* AllocationBase;
    uint32_t AllocationProtect;
    SIZE_T RegionSize;
    uint32_t State;
    uint32_t Protect;
    uint32_t Type;
};

struct STARTUPINFOW {
    DWORD cb;
    void* lpReserved;
    void* lpDesktop;
    void* lpTitle;
    DWORD dwX;
    DWORD dwY;
    DWORD dwXSize;
    DWORD dwYSize;
    DWORD dwXCountChars;
    DWORD dwYCountChars;
    DWORD dwFillAttribute;
    DWORD dwFlags;
    WORD wShowWindow;
    WORD cbReserved2;
    BYTE* lpReserved2;
    HANDLE hStdInput;
    HANDLE hStdOutput;
    HANDLE hStdError;
};

struct PROCESS_INFORMATION {
    HANDLE hProcess;
    HANDLE hThread;
    DWORD dwProcessId;
    DWORD dwThreadId;
};

struct CRITICAL_SECTION {
    void* DebugInfo;
    int32_t LockCount;
    int32_t RecursionCount;
    HANDLE OwningThread;
    HANDLE LockSemaphore;
    ULONG SpinCount;
};

struct SYSTEM_INFO {
    WORD wProcessorArchitecture;
    WORD wReserved;
    DWORD dwPageSize;
    void* lpMinimumApplicationAddress;
    void* lpMaximumApplicationAddress;
    DWORD_PTR dwActiveProcessorMask;
    DWORD dwNumberOfProcessors;
    DWORD dwProcessorType;
    DWORD dwAllocationGranularity;
    WORD wProcessorLevel;
    WORD wProcessorRevision;
};

constexpr uint32_t MEM_COMMIT = 0x1000;
constexpr uint32_t MEM_RESERVE = 0x2000;
constexpr uint32_t MEM_RELEASE = 0x8000;
constexpr uint32_t MEM_DECOMMIT = 0x4000;

constexpr uint32_t PAGE_NOACCESS = 0x01;
constexpr uint32_t PAGE_READONLY = 0x02;
constexpr uint32_t PAGE_READWRITE = 0x04;
constexpr uint32_t PAGE_EXECUTE = 0x10;
constexpr uint32_t PAGE_EXECUTE_READ = 0x20;
constexpr uint32_t PAGE_EXECUTE_READWRITE = 0x40;

constexpr uint32_t GENERIC_READ = 0x80000000;
constexpr uint32_t GENERIC_WRITE = 0x40000000;
constexpr uint32_t FILE_SHARE_READ = 0x00000001;

constexpr uint32_t CREATE_NEW = 1;
constexpr uint32_t CREATE_ALWAYS = 2;
constexpr uint32_t OPEN_EXISTING = 3;
constexpr uint32_t OPEN_ALWAYS = 4;
constexpr uint32_t TRUNCATE_EXISTING = 5;

constexpr uint32_t FILE_ATTRIBUTE_NORMAL = 0x80;

constexpr uint32_t STD_INPUT_HANDLE = 0xFFFFFFF6;
constexpr uint32_t STD_OUTPUT_HANDLE = 0xFFFFFFF5;
constexpr uint32_t STD_ERROR_HANDLE = 0xFFFFFFF4;

constexpr uint32_t INFINITE = 0xFFFFFFFF;

constexpr uint32_t WAIT_OBJECT_0 = 0;
constexpr uint32_t WAIT_TIMEOUT = 258;
constexpr uint32_t WAIT_FAILED = 0xFFFFFFFF;

} // namespace win32
} // namespace metalsharp
