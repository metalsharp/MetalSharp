/// @file UpdateChecker.cpp
/// @brief Semantic version parsing, comparison, and GitHub release checking.
///
/// Implements semver parsing and ordering logic to compare the current MetalSharp
/// version against the latest GitHub release. Parses release JSON to extract tag
/// names and download URLs without depending on an HTTP or JSON library.

#include "metalsharp/UpdateChecker.h"
#include <algorithm>
#include <cerrno>
#include <spawn.h>
#include <sstream>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

extern char** environ;

namespace metalsharp {

namespace {

std::vector<std::string> splitPrerelease(const std::string& value) {
    std::vector<std::string> identifiers;
    size_t start = 0;
    while (start <= value.size()) {
        size_t end = value.find('.', start);
        if (end == std::string::npos) {
            identifiers.push_back(value.substr(start));
            break;
        }
        identifiers.push_back(value.substr(start, end - start));
        start = end + 1;
    }
    return identifiers;
}

bool isNumericIdentifier(const std::string& identifier) {
    return !identifier.empty() &&
           std::all_of(identifier.begin(), identifier.end(), [](char c) { return c >= '0' && c <= '9'; });
}

int compareNumericIdentifiers(const std::string& left, const std::string& right) {
    const auto leftFirst = left.find_first_not_of('0');
    const auto rightFirst = right.find_first_not_of('0');
    const std::string leftNormalized = leftFirst == std::string::npos ? "0" : left.substr(leftFirst);
    const std::string rightNormalized = rightFirst == std::string::npos ? "0" : right.substr(rightFirst);

    if (leftNormalized.size() != rightNormalized.size())
        return leftNormalized.size() < rightNormalized.size() ? -1 : 1;
    if (leftNormalized == rightNormalized)
        return 0;
    return leftNormalized < rightNormalized ? -1 : 1;
}

int comparePrerelease(const std::string& left, const std::string& right) {
    if (left.empty() || right.empty()) {
        if (left.empty() && right.empty())
            return 0;
        return left.empty() ? 1 : -1;
    }

    const auto leftIdentifiers = splitPrerelease(left);
    const auto rightIdentifiers = splitPrerelease(right);
    const size_t sharedCount = std::min(leftIdentifiers.size(), rightIdentifiers.size());
    for (size_t i = 0; i < sharedCount; ++i) {
        const std::string& leftIdentifier = leftIdentifiers[i];
        const std::string& rightIdentifier = rightIdentifiers[i];
        if (leftIdentifier == rightIdentifier)
            continue;

        const bool leftNumeric = isNumericIdentifier(leftIdentifier);
        const bool rightNumeric = isNumericIdentifier(rightIdentifier);
        if (leftNumeric && rightNumeric)
            return compareNumericIdentifiers(leftIdentifier, rightIdentifier);
        if (leftNumeric != rightNumeric)
            return leftNumeric ? -1 : 1;
        return leftIdentifier < rightIdentifier ? -1 : 1;
    }

    if (leftIdentifiers.size() == rightIdentifiers.size())
        return 0;
    return leftIdentifiers.size() < rightIdentifiers.size() ? -1 : 1;
}

bool isValidRepository(const std::string& repo) {
    const size_t slash = repo.find('/');
    if (slash == std::string::npos || slash == 0 || slash + 1 >= repo.size() ||
        repo.find('/', slash + 1) != std::string::npos)
        return false;

    return std::all_of(repo.begin(), repo.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
               c == '.' || c == '/';
    });
}

} // namespace

bool Version::operator>(const Version& o) const {
    if (major != o.major)
        return major > o.major;
    if (minor != o.minor)
        return minor > o.minor;
    if (patch != o.patch)
        return patch > o.patch;
    return comparePrerelease(prerelease, o.prerelease) > 0;
}

bool Version::operator==(const Version& o) const {
    return major == o.major && minor == o.minor && patch == o.patch && comparePrerelease(prerelease, o.prerelease) == 0;
}

bool Version::operator>=(const Version& o) const {
    return *this > o || *this == o;
}

std::string Version::toString() const {
    std::string s = std::to_string(major) + "." + std::to_string(minor) + "." + std::to_string(patch);
    if (!prerelease.empty())
        s += "-" + prerelease;
    return s;
}

Version Version::parse(const std::string& s) {
    Version v;
    std::string input = s;
    if (input.size() > 0 && input[0] == 'v')
        input = input.substr(1);

    size_t buildPos = input.find('+');
    if (buildPos != std::string::npos)
        input = input.substr(0, buildPos);

    size_t dashPos = input.find('-');
    std::string versionPart = (dashPos != std::string::npos) ? input.substr(0, dashPos) : input;
    if (dashPos != std::string::npos)
        v.prerelease = input.substr(dashPos + 1);

    std::istringstream ss(versionPart);
    std::string token;
    int idx = 0;
    while (std::getline(ss, token, '.')) {
        try {
            uint32_t val = static_cast<uint32_t>(std::stoul(token));
            if (idx == 0)
                v.major = val;
            else if (idx == 1)
                v.minor = val;
            else if (idx == 2)
                v.patch = val;
        } catch (...) {
        }
        idx++;
    }

    return v;
}

Version UpdateChecker::getCurrentVersion() {
    return Version::parse("0.1.0");
}

std::string UpdateChecker::getUserAgent() {
    return "MetalSharp/0.1.0 (macOS)";
}

std::string UpdateChecker::fetchLatestRelease(const std::string& repo) {
    // The GitHub REST endpoint has no detached signature for its JSON response.
    // Keep the request limited to a validated repository over CA-verified HTTPS;
    // signed/package asset verification remains the release/installer's contract.
    if (!isValidRepository(repo))
        return "";

    const std::string url = "https://api.github.com/repos/" + repo + "/releases/latest";
    const std::string userAgent = getUserAgent();
    std::vector<std::string> arguments = {
        "/usr/bin/curl",
        "--fail",
        "--silent",
        "--proto",
        "=https",
        "--tlsv1.2",
        "--connect-timeout",
        "5",
        "--max-time",
        "15",
        "--header",
        "Accept: application/vnd.github+json",
        "--header",
        "X-GitHub-Api-Version: 2022-11-28",
        "--user-agent",
        userAgent,
        url,
    };
    std::vector<char*> argv;
    argv.reserve(arguments.size() + 1);
    for (auto& argument : arguments)
        argv.push_back(argument.data());
    argv.push_back(nullptr);

    int outputPipe[2];
    if (pipe(outputPipe) != 0)
        return "";

    posix_spawn_file_actions_t actions;
    if (posix_spawn_file_actions_init(&actions) != 0) {
        close(outputPipe[0]);
        close(outputPipe[1]);
        return "";
    }
    int actionResult = posix_spawn_file_actions_adddup2(&actions, outputPipe[1], STDOUT_FILENO);
    if (actionResult == 0)
        actionResult = posix_spawn_file_actions_addclose(&actions, outputPipe[0]);
    if (actionResult == 0)
        actionResult = posix_spawn_file_actions_addclose(&actions, outputPipe[1]);
    if (actionResult != 0) {
        posix_spawn_file_actions_destroy(&actions);
        close(outputPipe[0]);
        close(outputPipe[1]);
        return "";
    }

    pid_t child = 0;
    const int spawnResult = posix_spawn(&child, arguments[0].c_str(), &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    close(outputPipe[1]);
    if (spawnResult != 0) {
        close(outputPipe[0]);
        return "";
    }

    constexpr size_t maxResponseBytes = 4 * 1024 * 1024;
    std::string response;
    char buffer[4096];
    bool tooLarge = false;
    bool readFailed = false;
    while (true) {
        const ssize_t bytesRead = read(outputPipe[0], buffer, sizeof(buffer));
        if (bytesRead == 0)
            break;
        if (bytesRead < 0) {
            if (errno == EINTR)
                continue;
            readFailed = true;
            break;
        }
        if (response.size() + static_cast<size_t>(bytesRead) <= maxResponseBytes)
            response.append(buffer, static_cast<size_t>(bytesRead));
        else
            tooLarge = true;
    }
    close(outputPipe[0]);

    int status = 0;
    pid_t waited;
    do {
        waited = waitpid(child, &status, 0);
    } while (waited < 0 && errno == EINTR);
    if (readFailed || tooLarge || waited < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0)
        return "";
    return response;
}

UpdateInfo UpdateChecker::parseGitHubRelease(const std::string& json, const Version& current) {
    UpdateInfo info;
    info.currentVersion = current.toString();

    auto extractJsonString = [](const std::string& j, const std::string& key) -> std::string {
        std::string search = "\"" + key + "\"";
        auto pos = j.find(search);
        if (pos == std::string::npos)
            return "";
        auto colon = j.find(':', pos + search.length());
        if (colon == std::string::npos)
            return "";
        auto openQuote = colon + 1;
        while (openQuote < j.size() &&
               (j[openQuote] == ' ' || j[openQuote] == '\n' || j[openQuote] == '\r' || j[openQuote] == '\t'))
            ++openQuote;
        if (openQuote >= j.size() || j[openQuote] != '"')
            return "";

        std::string value;
        bool escaped = false;
        for (size_t i = openQuote + 1; i < j.size(); ++i) {
            const char c = j[i];
            if (escaped) {
                escaped = false;
                switch (c) {
                case '"':
                case '\\':
                case '/':
                    value += c;
                    break;
                case 'b':
                    value += '\b';
                    break;
                case 'f':
                    value += '\f';
                    break;
                case 'n':
                    value += '\n';
                    break;
                case 'r':
                    value += '\r';
                    break;
                case 't':
                    value += '\t';
                    break;
                case 'u':
                    if (i + 4 >= j.size())
                        return "";
                    value += "\\u";
                    value.append(j, i + 1, 4);
                    i += 4;
                    break;
                default:
                    return "";
                }
            } else if (c == '\\') {
                escaped = true;
            } else if (c == '"') {
                return value;
            } else {
                value += c;
            }
        }
        return "";
    };

    std::string tag = extractJsonString(json, "tag_name");
    if (!tag.empty()) {
        info.latestVersion = Version::parse(tag);
        info.available = info.latestVersion > current;
    }

    info.downloadUrl = extractJsonString(json, "zipball_url");
    info.releaseNotes = extractJsonString(json, "body");
    info.htmlUrl = extractJsonString(json, "html_url");

    return info;
}

UpdateInfo UpdateChecker::checkForUpdates(const std::string& repo) {
    Version current = getCurrentVersion();
    std::string json = fetchLatestRelease(repo);
    if (json.empty()) {
        UpdateInfo info;
        info.currentVersion = current.toString();
        info.available = false;
        return info;
    }
    return parseGitHubRelease(json, current);
}

} // namespace metalsharp
