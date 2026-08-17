/// @file UpdateChecker.h
/// @brief GitHub release checker for MetalSharp version updates.
///
/// Queries the GitHub releases API for the latest MetalSharp version, parses semver
/// tags, and compares against the current build version. Returns an UpdateInfo struct
/// with download URL, release notes, and availability flag. The SettingsManager's
/// auto-check feature calls this at startup when enabled.

#pragma once

#include <cstdint>
#include <string>

namespace metalsharp {

struct Version {
    uint32_t major = 0;
    uint32_t minor = 0;
    uint32_t patch = 0;
    std::string prerelease;

    bool operator>(const Version& o) const;
    bool operator==(const Version& o) const;
    bool operator>=(const Version& o) const;
    std::string toString() const;
    static Version parse(const std::string& s);
};

struct UpdateInfo {
    bool available = false;
    Version latestVersion;
    std::string currentVersion;
    std::string downloadUrl;
    std::string releaseNotes;
    std::string htmlUrl;
};

class UpdateChecker {
  public:
    static UpdateInfo checkForUpdates(const std::string& repo = "metalsharp/MetalSharp");
    static Version getCurrentVersion();
    static std::string getUserAgent();
    static UpdateInfo parseGitHubRelease(const std::string& json, const Version& current);

  private:
    static std::string fetchLatestRelease(const std::string& repo);
};

} // namespace metalsharp
