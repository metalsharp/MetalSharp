/// @file Win32Error.h
/// @brief Single shared thread-local Win32 last-error state for the shim layer.
///
/// Every kernel32 shim and the VirtualFileSystem translation layer report
/// failures through one thread-local variable (`t_lastError`), surfaced to PE
/// code as GetLastError()/SetLastError(). Failure paths set errno-derived
/// Win32 codes via setLastErrorFromErrno() so games that gate on error codes
/// observe the real failure instead of ERROR_SUCCESS.
#pragma once

#include <cerrno>
#include <cstdint>

#include "Win32Types.h"

namespace metalsharp {
namespace win32 {

// Win32 error codes used by the shim layer (values from winerror.h).
constexpr DWORD ERROR_SUCCESS = 0;
constexpr DWORD ERROR_INVALID_FUNCTION = 1;
constexpr DWORD ERROR_FILE_NOT_FOUND = 2;
constexpr DWORD ERROR_PATH_NOT_FOUND = 3;
constexpr DWORD ERROR_TOO_MANY_OPEN_FILES = 4;
constexpr DWORD ERROR_ACCESS_DENIED = 5;
constexpr DWORD ERROR_INVALID_HANDLE = 6;
constexpr DWORD ERROR_NOT_ENOUGH_MEMORY = 8;
constexpr DWORD ERROR_INVALID_ACCESS = 12;
constexpr DWORD ERROR_OUTOFMEMORY = 14;
constexpr DWORD ERROR_NOT_SAME_DEVICE = 17;
constexpr DWORD ERROR_NO_MORE_FILES = 18;
constexpr DWORD ERROR_WRITE_PROTECT = 19;
constexpr DWORD ERROR_SEEK = 25;
constexpr DWORD ERROR_WRITE_FAULT = 29;
constexpr DWORD ERROR_GEN_FAILURE = 31;
constexpr DWORD ERROR_SHARING_VIOLATION = 32;
constexpr DWORD ERROR_HANDLE_DISK_FULL = 39;
constexpr DWORD ERROR_NOT_SUPPORTED = 50;
constexpr DWORD ERROR_CALL_NOT_IMPLEMENTED = 120;
constexpr DWORD ERROR_INSUFFICIENT_BUFFER = 122;
constexpr DWORD ERROR_INVALID_NAME = 123;
constexpr DWORD ERROR_MOD_NOT_FOUND = 126;
constexpr DWORD ERROR_PROC_NOT_FOUND = 127;
constexpr DWORD ERROR_DISK_FULL = 112;
constexpr DWORD ERROR_DIR_NOT_EMPTY = 145;
constexpr DWORD ERROR_ALREADY_EXISTS = 183;
constexpr DWORD ERROR_FILENAME_EXCED_RANGE = 206;
constexpr DWORD ERROR_INVALID_ADDRESS = 487;
constexpr DWORD ERROR_OPERATION_ABORTED = 995;
constexpr DWORD ERROR_TOO_MANY_LINKS = 1142;
constexpr DWORD ERROR_TIMEOUT = 1460;

/// Map a POSIX errno value to the closest Win32 error code.
inline DWORD errnoToWin32(int err) {
    switch (err) {
    case ENOENT:
        return ERROR_FILE_NOT_FOUND;
    case EACCES:
    case EPERM:
        return ERROR_ACCESS_DENIED;
    case EBADF:
        return ERROR_INVALID_HANDLE;
    case EEXIST:
        return ERROR_ALREADY_EXISTS;
    case ENOTDIR:
        return ERROR_PATH_NOT_FOUND;
    case EISDIR:
        return ERROR_ACCESS_DENIED;
    case EINVAL:
        return ERROR_INVALID_PARAMETER;
    case ENOMEM:
        return ERROR_NOT_ENOUGH_MEMORY;
    case EMFILE:
    case ENFILE:
        return ERROR_TOO_MANY_OPEN_FILES;
    case ENOSPC:
    case EDQUOT:
        return ERROR_DISK_FULL;
    case EROFS:
        return ERROR_WRITE_PROTECT;
    case ESPIPE:
        return ERROR_SEEK;
    case EFBIG:
        return ERROR_WRITE_FAULT;
    case EIO:
        return ERROR_GEN_FAILURE;
    case ENAMETOOLONG:
        return ERROR_FILENAME_EXCED_RANGE;
    case ELOOP:
        return ERROR_TOO_MANY_LINKS;
    case EINTR:
        return ERROR_OPERATION_ABORTED;
    case ERANGE:
        return ERROR_INSUFFICIENT_BUFFER;
    case ENOTEMPTY:
        return ERROR_DIR_NOT_EMPTY;
    case EXDEV:
        return ERROR_NOT_SAME_DEVICE;
    case EOPNOTSUPP:
        return ERROR_NOT_SUPPORTED;
    case ETIMEDOUT:
        return ERROR_TIMEOUT;
    default:
        return ERROR_GEN_FAILURE;
    }
}

/// The single shared thread-local last-error variable backing
/// GetLastError()/SetLastError() across all shim translation units.
inline thread_local DWORD t_lastError = 0;

inline DWORD lastErrorCode() {
    return t_lastError;
}

inline void setLastErrorCode(DWORD code) {
    t_lastError = code;
}

/// Record the current errno, mapped to a Win32 error code. Call at the point
/// of failure while errno still describes the failing syscall.
inline void setLastErrorFromErrno() {
    t_lastError = errnoToWin32(errno);
}

} // namespace win32
} // namespace metalsharp
