#pragma once

#include <filesystem>
#include <fstream>
#include <sys/stat.h>

namespace metalsharp::runtime_detail {

namespace fs = std::filesystem;

class ScopedPrivateUmask final {
  public:
    ScopedPrivateUmask() : m_previous(umask(0077)) {}
    ~ScopedPrivateUmask() { umask(m_previous); }

    ScopedPrivateUmask(const ScopedPrivateUmask&) = delete;
    ScopedPrivateUmask& operator=(const ScopedPrivateUmask&) = delete;

  private:
    mode_t m_previous;
};

// Apply the mode to both newly created and pre-existing paths. The latter is
// important when a directory was created by an older, permissive build.
inline bool ensurePrivateDirectory(const fs::path& directory) {
    if (directory.empty())
        return false;

    std::error_code ec;
    {
        ScopedPrivateUmask privateUmask;
        fs::create_directories(directory, ec);
    }
    if (ec || !fs::is_directory(directory, ec) || ec)
        return false;

    fs::permissions(directory, fs::perms::owner_all, fs::perm_options::replace, ec);
    return !ec;
}

inline bool openPrivateFile(std::ofstream& file, const fs::path& path) {
    {
        ScopedPrivateUmask privateUmask;
        file.open(path, std::ios::out | std::ios::trunc);
    }
    if (!file.is_open())
        return false;

    std::error_code ec;
    fs::permissions(path, fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::replace, ec);
    if (ec) {
        file.close();
        return false;
    }
    return true;
}

} // namespace metalsharp::runtime_detail
