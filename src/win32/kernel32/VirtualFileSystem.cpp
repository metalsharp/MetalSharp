/// @file VirtualFileSystem.cpp
/// @brief Win32-to-Unix file system translation layer.
///
/// Translates Win32 path conventions (backslashes, drive letters) to POSIX paths and implements CreateFile, ReadFile,
/// WriteFile, FindFirstFile/FindNextFile. Manages virtual file handles and maps Win32 file attributes to Unix stat
/// results.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <metalsharp/Logger.h>
#include <metalsharp/VirtualFileSystem.h>
#include <metalsharp/Win32Error.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <sys/stat.h>
#include <sys/types.h>

namespace metalsharp {
namespace win32 {

static std::string toLower(std::string s) {
    for (auto& c : s)
        c = tolower(c);
    return s;
}

static bool mkdirRecursive(const std::string& path) {
    size_t pos = path.rfind('/');
    if (pos != std::string::npos && pos > 0) {
        std::string parent = path.substr(0, pos);
        struct stat st;
        if (stat(parent.c_str(), &st) != 0) {
            mkdirRecursive(parent);
        }
    }
    mkdir(path.c_str(), 0755);
    return true;
}

static std::string ensureDir(const std::string& path) {
    size_t pos = path.rfind('/');
    if (pos != std::string::npos) {
        std::string dir = path.substr(0, pos);
        struct stat st;
        if (stat(dir.c_str(), &st) != 0) {
            mkdirRecursive(dir);
        }
    }
    return path;
}

VirtualFileSystem::VirtualFileSystem() {
    const char* home = getenv("HOME");
    if (home) {
        m_prefix = std::string(home) + "/.metalsharp/prefix";
    } else {
        m_prefix = "/tmp/metalsharp/prefix";
    }
    ensureDir(m_prefix + "/drive_c/Users/user");
    ensureDir(m_prefix + "/drive_c/Program Files");
    ensureDir(m_prefix + "/drive_c/Windows");
    ensureDir(m_prefix + "/drive_c/Windows/System32");
}

VirtualFileSystem& VirtualFileSystem::instance() {
    static VirtualFileSystem vfs;
    return vfs;
}

void VirtualFileSystem::setPrefix(const std::string& prefix) {
    m_prefix = prefix;
    ensureDir(m_prefix + "/drive_c/Users/user");
    ensureDir(m_prefix + "/drive_c/Program Files");
    ensureDir(m_prefix + "/drive_c/Windows");
    ensureDir(m_prefix + "/drive_c/Windows/System32");
}

std::string VirtualFileSystem::winToHost(const std::string& winPath) {
    if (winPath.empty())
        return "";

    std::string path = winPath;

    if (path.find("\\\\?\\") == 0) {
        path = path.substr(4);
    }

    for (auto& c : path) {
        if (c == '\\')
            c = '/';
    }

    if (path.size() >= 2 && path[1] == ':') {
        char drive = tolower(path[0]);
        if (drive >= 'a' && drive <= 'z') {
            if (drive == 'c') {
                path = m_prefix + "/drive_c" + path.substr(2);
            } else {
                path = m_prefix + "/drive_" + std::string(1, drive) + path.substr(2);
            }
        }
    } else if (path[0] == '/' && path.find("/drive_") == std::string::npos && path.find(m_prefix) != 0) {
        return path;
    }

    while (path.find("//") != std::string::npos) {
        size_t pos = path.find("//");
        path.erase(pos, 1);
    }

    return path;
}

std::string VirtualFileSystem::hostToWin(const std::string& hostPath) {
    if (hostPath.find(m_prefix) == 0) {
        std::string rel = hostPath.substr(m_prefix.size());
        if (rel.find("/drive_c") == 0) {
            return "C:" + rel.substr(8);
        } else if (rel.size() > 7 && rel.substr(1, 6) == "drive_") {
            char drive = toupper(rel[7]);
            return std::string(1, drive) + ":" + rel.substr(8);
        }
    }
    return hostPath;
}

HANDLE VirtualFileSystem::allocHandle(HandleType type, void* data) {
    uintptr_t h = m_nextHandle;
    m_nextHandle += 4;
    if (m_nextHandle < 0x00010000)
        m_nextHandle = 0x00010000;
    m_handles[h] = {type, data};
    return reinterpret_cast<HANDLE>(h);
}

HandleEntry* VirtualFileSystem::getHandle(HANDLE h) {
    auto it = m_handles.find(reinterpret_cast<uintptr_t>(h));
    if (it == m_handles.end())
        return nullptr;
    return &it->second;
}

bool VirtualFileSystem::closeHandle(HANDLE h) {
    auto it = m_handles.find(reinterpret_cast<uintptr_t>(h));
    if (it == m_handles.end()) {
        setLastErrorCode(ERROR_INVALID_HANDLE);
        return false;
    }

    HandleEntry& entry = it->second;
    switch (entry.type) {
    case HandleType::File: {
        auto* fs = static_cast<FileState*>(entry.data);
        if (fs->fd >= 0)
            close(fs->fd);
        delete fs;
        break;
    }
    case HandleType::Find: {
        auto* fnd = static_cast<FindState*>(entry.data);
        if (fnd->dir)
            closedir(fnd->dir);
        delete fnd;
        break;
    }
    default:
        break;
    }

    m_handles.erase(it);
    return true;
}

HANDLE VirtualFileSystem::createFile(const char* lpFileName, DWORD dwDesiredAccess, DWORD dwShareMode,
                                     DWORD dwCreationDisposition, DWORD dwFlagsAndAttributes) {
    if (!lpFileName) {
        setLastErrorCode(ERROR_INVALID_PARAMETER);
        return INVALID_HANDLE_VALUE;
    }

    std::string hostPath = winToHost(lpFileName);

    MS_INFO("VFS: CreateFile(\"%s\" -> \"%s\", access=0x%X, disp=%u)", lpFileName, hostPath.c_str(), dwDesiredAccess,
            dwCreationDisposition);

    int flags = 0;
    bool readOnly = !(dwDesiredAccess & 0x40000000);
    bool writeAccess = (dwDesiredAccess & 0x40000000) != 0;
    bool readAccess = (dwDesiredAccess & 0x80000000) != 0;

    if (readAccess && writeAccess)
        flags = O_RDWR;
    else if (writeAccess)
        flags = O_WRONLY;
    else if (readAccess)
        flags = O_RDONLY;
    else
        flags = O_RDONLY;

    switch (dwCreationDisposition) {
    case 1:
        flags |= O_CREAT | O_EXCL;
        break;
    case 2:
        flags |= O_CREAT | O_TRUNC;
        break;
    case 3:
        break;
    case 4:
        flags |= O_CREAT;
        break;
    case 5:
        flags |= O_TRUNC;
        break;
    }

    if (writeAccess || (flags & O_CREAT)) {
        ensureDir(hostPath);
    }

    int fd = open(hostPath.c_str(), flags, 0644);
    if (fd < 0) {
        MS_INFO("VFS: CreateFile failed: %s (errno=%d)", strerror(errno), errno);
        setLastErrorFromErrno();
        return INVALID_HANDLE_VALUE;
    }

    auto* fs = new FileState{fd, hostPath, 0};
    return allocHandle(HandleType::File, fs);
}

BOOL VirtualFileSystem::readFile(HANDLE hFile, void* lpBuffer, DWORD nNumberOfBytesToRead, DWORD* lpNumberOfBytesRead) {
    auto* entry = getHandle(hFile);
    if (!entry || entry->type != HandleType::File) {
        setLastErrorCode(ERROR_INVALID_HANDLE);
        return 0;
    }

    auto* fs = static_cast<FileState*>(entry->data);
    ssize_t bytesRead = read(fs->fd, lpBuffer, nNumberOfBytesToRead);
    if (bytesRead < 0) {
        if (lpNumberOfBytesRead)
            *lpNumberOfBytesRead = 0;
        setLastErrorFromErrno();
        return 0;
    }

    fs->position += bytesRead;
    if (lpNumberOfBytesRead)
        *lpNumberOfBytesRead = static_cast<DWORD>(bytesRead);
    return 1;
}

BOOL VirtualFileSystem::writeFile(HANDLE hFile, const void* lpBuffer, DWORD nNumberOfBytesToWrite,
                                  DWORD* lpNumberOfBytesWritten) {
    auto* entry = getHandle(hFile);
    if (!entry || entry->type != HandleType::File) {
        setLastErrorCode(ERROR_INVALID_HANDLE);
        return 0;
    }

    auto* fs = static_cast<FileState*>(entry->data);
    ssize_t bytesWritten = write(fs->fd, lpBuffer, nNumberOfBytesToWrite);
    if (bytesWritten < 0) {
        if (lpNumberOfBytesWritten)
            *lpNumberOfBytesWritten = 0;
        setLastErrorFromErrno();
        return 0;
    }

    fs->position += bytesWritten;
    if (lpNumberOfBytesWritten)
        *lpNumberOfBytesWritten = static_cast<DWORD>(bytesWritten);
    return 1;
}

DWORD VirtualFileSystem::getFileSize(HANDLE hFile, DWORD* lpFileSizeHigh) {
    auto* entry = getHandle(hFile);
    if (!entry || entry->type != HandleType::File) {
        if (lpFileSizeHigh)
            *lpFileSizeHigh = 0;
        setLastErrorCode(ERROR_INVALID_HANDLE);
        return 0xFFFFFFFF;
    }

    auto* fs = static_cast<FileState*>(entry->data);
    struct stat st;
    if (fstat(fs->fd, &st) != 0) {
        if (lpFileSizeHigh)
            *lpFileSizeHigh = 0;
        setLastErrorFromErrno();
        return 0xFFFFFFFF;
    }

    if (lpFileSizeHigh)
        *lpFileSizeHigh = static_cast<DWORD>(st.st_size >> 32);
    return static_cast<DWORD>(st.st_size & 0xFFFFFFFF);
}

BOOL VirtualFileSystem::getFileSizeEx(HANDLE hFile, int64_t* lpFileSize) {
    auto* entry = getHandle(hFile);
    if (!entry || entry->type != HandleType::File) {
        setLastErrorCode(ERROR_INVALID_HANDLE);
        return 0;
    }

    auto* fs = static_cast<FileState*>(entry->data);
    struct stat st;
    if (fstat(fs->fd, &st) != 0) {
        setLastErrorFromErrno();
        return 0;
    }

    if (lpFileSize)
        *lpFileSize = st.st_size;
    return 1;
}

DWORD VirtualFileSystem::setFilePointer(HANDLE hFile, LONG lDistanceToMove, LONG* lpDistanceToMoveHigh,
                                        DWORD dwMoveMethod) {
    auto* entry = getHandle(hFile);
    if (!entry || entry->type != HandleType::File) {
        setLastErrorCode(ERROR_INVALID_HANDLE);
        return 0xFFFFFFFF;
    }

    auto* fs = static_cast<FileState*>(entry->data);
    int whence;
    switch (dwMoveMethod) {
    case 0:
        whence = SEEK_SET;
        break;
    case 1:
        whence = SEEK_CUR;
        break;
    case 2:
        whence = SEEK_END;
        break;
    default:
        setLastErrorCode(ERROR_INVALID_PARAMETER);
        return 0xFFFFFFFF;
    }

    off_t offset = lDistanceToMove;
    if (lpDistanceToMoveHigh) {
        int64_t full = (static_cast<int64_t>(*lpDistanceToMoveHigh) << 32) | (static_cast<uint32_t>(lDistanceToMove));
        offset = full;
    }

    off_t result = lseek(fs->fd, offset, whence);
    if (result == (off_t)-1) {
        setLastErrorFromErrno();
        return 0xFFFFFFFF;
    }

    fs->position = result;
    return static_cast<DWORD>(result & 0xFFFFFFFF);
}

BOOL VirtualFileSystem::setFilePointerEx(HANDLE hFile, int64_t liDistanceToMove, int64_t* lpNewFilePointer,
                                         DWORD dwMoveMethod) {
    auto* entry = getHandle(hFile);
    if (!entry || entry->type != HandleType::File) {
        setLastErrorCode(ERROR_INVALID_HANDLE);
        return 0;
    }

    auto* fs = static_cast<FileState*>(entry->data);
    int whence;
    switch (dwMoveMethod) {
    case 0:
        whence = SEEK_SET;
        break;
    case 1:
        whence = SEEK_CUR;
        break;
    case 2:
        whence = SEEK_END;
        break;
    default:
        setLastErrorCode(ERROR_INVALID_PARAMETER);
        return 0;
    }

    off_t result = lseek(fs->fd, liDistanceToMove, whence);
    if (result == (off_t)-1) {
        setLastErrorFromErrno();
        return 0;
    }

    fs->position = result;
    if (lpNewFilePointer)
        *lpNewFilePointer = result;
    return 1;
}

BOOL VirtualFileSystem::flushFileBuffers(HANDLE hFile) {
    auto* entry = getHandle(hFile);
    if (!entry || entry->type != HandleType::File) {
        setLastErrorCode(ERROR_INVALID_HANDLE);
        return 0;
    }

    auto* fs = static_cast<FileState*>(entry->data);
    if (fsync(fs->fd) != 0) {
        setLastErrorFromErrno();
        return 0;
    }
    return 1;
}

DWORD VirtualFileSystem::getFileType(HANDLE hFile) {
    auto* entry = getHandle(hFile);
    if (!entry || entry->type != HandleType::File) {
        setLastErrorCode(ERROR_INVALID_HANDLE);
        return 0;
    }

    auto* fs = static_cast<FileState*>(entry->data);
    struct stat st;
    if (fstat(fs->fd, &st) != 0)
        return 0;

    if (S_ISREG(st.st_mode))
        return 0x0001;
    if (S_ISCHR(st.st_mode))
        return 0x0002;
    if (S_ISDIR(st.st_mode))
        return 0x0001;
    return 0x0001;
}

static void fillWin32FindData(void* lpFindFileData, const char* name, bool isDir, int64_t fileSize) {
    uint8_t* data = static_cast<uint8_t*>(lpFindFileData);
    DWORD attrs = 0x80;
    if (isDir)
        attrs |= 0x10;
    if (name[0] == '.')
        attrs |= 0x02;
    memcpy(data, &attrs, 4);

    uint64_t ft = 0;
    memcpy(data + 4, &ft, 8);
    memcpy(data + 12, &ft, 8);
    memcpy(data + 20, &ft, 8);

    uint64_t fsize = static_cast<uint64_t>(fileSize);
    memcpy(data + 28, &fsize, 8);

    // WIN32_FIND_DATAW places cFileName after dwReserved0 and dwReserved1 at byte 44.
    // Leave those reserved fields untouched and initialize the complete file-name field.
    constexpr size_t cFileNameOffset = 44;
    constexpr size_t cFileNameCapacity = 260;
    memset(data + cFileNameOffset, 0, cFileNameCapacity * sizeof(uint16_t));
    for (size_t i = 0; name[i] && i + 1 < cFileNameCapacity; i++) {
        uint16_t codeUnit = static_cast<uint16_t>(static_cast<unsigned char>(name[i]));
        memcpy(data + cFileNameOffset + i * sizeof(codeUnit), &codeUnit, sizeof(codeUnit));
    }
}

HANDLE VirtualFileSystem::findFirstFileW(const char* pattern, void* lpFindFileData) {
    std::string hostPattern = winToHost(pattern);

    std::string dir;
    std::string glob;
    auto lastSlash = hostPattern.rfind('/');
    if (lastSlash != std::string::npos) {
        dir = hostPattern.substr(0, lastSlash);
        glob = hostPattern.substr(lastSlash + 1);
    } else {
        dir = ".";
        glob = hostPattern;
    }

    if (glob.empty())
        glob = "*";

    DIR* d = opendir(dir.c_str());
    if (!d) {
        MS_INFO("VFS: FindFirstFile failed to open dir: %s", dir.c_str());
        setLastErrorFromErrno();
        return INVALID_HANDLE_VALUE;
    }

    auto* fnd = new FindState{d, glob, dir, false};

    HANDLE h = allocHandle(HandleType::Find, fnd);
    if (findNextFileW(h, lpFindFileData)) {
        return h;
    }

    closeHandle(h);
    return INVALID_HANDLE_VALUE;
}

BOOL VirtualFileSystem::findNextFileW(HANDLE hFindFile, void* lpFindFileData) {
    auto* entry = getHandle(hFindFile);
    if (!entry || entry->type != HandleType::Find) {
        setLastErrorCode(ERROR_INVALID_HANDLE);
        return 0;
    }

    auto* fnd = static_cast<FindState*>(entry->data);
    if (!fnd->dir) {
        setLastErrorCode(ERROR_INVALID_HANDLE);
        return 0;
    }

    struct dirent* de;
    while ((de = readdir(fnd->dir)) != nullptr) {
        if (fnmatch(fnd->pattern.c_str(), de->d_name, FNM_NOESCAPE) == 0) {
            std::string fullPath = fnd->directory + "/" + de->d_name;
            struct stat st;
            bool isDir = false;
            int64_t fileSize = 0;
            if (stat(fullPath.c_str(), &st) == 0) {
                isDir = S_ISDIR(st.st_mode);
                fileSize = st.st_size;
            }
            fillWin32FindData(lpFindFileData, de->d_name, isDir, fileSize);
            return 1;
        }
    }

    // Listing exhausted: report the Win32 end-of-search condition.
    setLastErrorCode(ERROR_FILE_NOT_FOUND);
    return 0;
}

BOOL VirtualFileSystem::findClose(HANDLE hFindFile) {
    return closeHandle(hFindFile) ? 1 : 0;
}

DWORD VirtualFileSystem::getFileAttributes(const std::string& path) {
    std::string host = winToHost(path);
    struct stat st;
    if (stat(host.c_str(), &st) != 0) {
        setLastErrorFromErrno();
        return 0xFFFFFFFF;
    }

    DWORD attrs = 0x80;
    if (S_ISDIR(st.st_mode))
        attrs |= 0x10;
    if (path[0] == '.' || path.find("/.") != std::string::npos)
        attrs |= 0x02;
    if (!(st.st_mode & S_IWUSR))
        attrs |= 0x01;
    return attrs;
}

BOOL VirtualFileSystem::getFileAttributesEx(const std::string& path, void* lpFileInformation) {
    std::string host = winToHost(path);
    struct stat st;
    if (stat(host.c_str(), &st) != 0) {
        setLastErrorFromErrno();
        return 0;
    }

    uint8_t* data = static_cast<uint8_t*>(lpFileInformation);
    DWORD attrs = 0x80;
    if (S_ISDIR(st.st_mode))
        attrs |= 0x10;
    memcpy(data, &attrs, 4);

    uint64_t ft = static_cast<uint64_t>(st.st_mtime) * 10000000ULL + 116444736000000000ULL;
    memcpy(data + 4, &ft, 8);
    memcpy(data + 12, &ft, 8);
    memcpy(data + 20, &ft, 8);
    uint64_t fsize = static_cast<uint64_t>(st.st_size);
    memcpy(data + 28, &fsize, 8);

    return 1;
}

BOOL VirtualFileSystem::getFileInformationByHandle(HANDLE hFile, void* lpFileInformation) {
    auto* entry = getHandle(hFile);
    if (!entry || entry->type != HandleType::File) {
        setLastErrorCode(ERROR_INVALID_HANDLE);
        return 0;
    }

    auto* fs = static_cast<FileState*>(entry->data);
    struct stat st;
    if (fstat(fs->fd, &st) != 0) {
        setLastErrorFromErrno();
        return 0;
    }

    uint8_t* data = static_cast<uint8_t*>(lpFileInformation);
    memset(data, 0, 52);
    DWORD attrs = 0x80;
    if (S_ISDIR(st.st_mode))
        attrs |= 0x10;
    memcpy(data, &attrs, 4);
    uint64_t ft = static_cast<uint64_t>(st.st_mtime) * 10000000ULL + 116444736000000000ULL;
    memcpy(data + 4, &ft, 8);
    memcpy(data + 12, &ft, 8);
    memcpy(data + 20, &ft, 8);
    uint64_t fsize = static_cast<uint64_t>(st.st_size);
    memcpy(data + 28, &fsize, 8);
    DWORD serial = 0x12345678;
    memcpy(data + 36, &serial, 4);
    DWORD linkCount = static_cast<DWORD>(st.st_nlink);
    memcpy(data + 44, &linkCount, 4);

    return 1;
}

std::string VirtualFileSystem::getFullPathName(const std::string& path) {
    if (path.empty())
        return "";

    std::string result = path;
    for (auto& c : result) {
        if (c == '\\')
            c = '/';
    }

    if (result.size() >= 2 && result[1] == ':') {
        return path;
    }

    if (result[0] == '/' || result[0] == '\\') {
        return "C:" + result;
    }

    return "C:\\Users\\user\\" + result;
}

HANDLE VirtualFileSystem::registerPipeFd(int fd) {
    auto* state = new FileState();
    state->fd = fd;
    state->position = 0;
    state->path = "<pipe>";
    return allocHandle(HandleType::Pipe, state);
}

} // namespace win32
} // namespace metalsharp
