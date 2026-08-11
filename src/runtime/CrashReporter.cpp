/// @file CrashReporter.cpp
/// @brief Crash report generation with file-based persistence.
///
/// Assembles structured crash reports including stack traces, system info, and
/// runtime state, then writes them to a reports directory on disk. Designed to
/// be safe to call from signal handlers and other constrained contexts.

#include "metalsharp/CrashReporter.h"
#include "PrivateStorage.h"
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <pwd.h>
#include <random>
#include <sstream>
#include <unistd.h>
#include <vector>

namespace metalsharp {

namespace fs = std::filesystem;

namespace {

// Launchers do not always inherit HOME. Resolve the account's home directory
// without falling back to a shared, world-writable temporary directory.
std::string passwdHomeDirectory() {
    long bufferSize = sysconf(_SC_GETPW_R_SIZE_MAX);
    if (bufferSize <= 0)
        bufferSize = 16384;

    std::vector<char> buffer(static_cast<size_t>(bufferSize));
    struct passwd passwordEntry{};
    struct passwd* result = nullptr;
    if (getpwuid_r(getuid(), &passwordEntry, buffer.data(), buffer.size(), &result) == 0 && result && result->pw_dir) {
        return result->pw_dir;
    }
    return {};
}

std::string applicationSupportDirectory() {
    const std::string home = passwdHomeDirectory();
    if (!home.empty())
        return home + "/Library/Application Support/MetalSharp";
    return "/Library/Application Support/MetalSharp";
}

std::string dataDirectory() {
    const char* home = std::getenv("HOME");
    if (home && *home)
        return std::string(home) + "/.metalsharp";
    return applicationSupportDirectory();
}

} // namespace

CrashReporter& CrashReporter::instance() {
    static CrashReporter inst;
    return inst;
}

std::string CrashReporter::generateId() {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<uint32_t> dist(0, 0xFFFFFFFF);
    uint32_t val = dist(gen);
    char buf[17];
    snprintf(buf, sizeof(buf), "%08x", val);
    return std::string(buf);
}

std::string CrashReporter::formatTimestamp(int64_t epoch) {
    time_t t = static_cast<time_t>(epoch);
    struct tm* tm_info = localtime(&t);
    char buf[64] = {};
    if (!tm_info || strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", tm_info) == 0)
        return {};
    return std::string(buf);
}

void CrashReporter::beginSession(const std::string& gameName, const std::string& exePath) {
    m_currentGame = gameName;
    m_currentExe = exePath;
    m_sessionStart = static_cast<int64_t>(std::time(nullptr));
    m_inSession = true;
}

void CrashReporter::endSession(int exitCode) {
    if (exitCode != 0 && m_inSession) {
        auto report = collectReport();
        report.exitCode = exitCode;
        saveReport(report);
    }
    m_inSession = false;
    m_currentGame.clear();
    m_currentExe.clear();
}

std::string CrashReporter::getReportsDir() {
    return dataDirectory() + "/crashes";
}

std::string CrashReporter::collectSystemInfo() {
    std::ostringstream ss;

#ifdef __APPLE__
    ss << "Platform: macOS\n";
#endif

    ss << "Architecture: ";
#ifdef __x86_64__
    ss << "x86_64\n";
#elif defined(__aarch64__) || defined(__arm64__)
    ss << "arm64\n";
#else
    ss << "unknown\n";
#endif

    ss << "Pointer size: " << sizeof(void*) * 8 << " bit\n";

    ss << "Home: [redacted]\n";

    return ss.str();
}

CrashReport CrashReporter::collectReport() {
    CrashReport report;
    report.id = generateId();
    report.timestamp = formatTimestamp(static_cast<int64_t>(std::time(nullptr)));
    report.gameName = m_currentGame;
    report.exePath = m_currentExe;
    report.systemInfo = collectSystemInfo();
    report.collected = true;

    const std::string base = dataDirectory();

    auto readFileIfExists = [&](const std::string& path) -> std::string {
        std::ifstream f(path);
        if (!f.is_open())
            return "";
        std::ostringstream buf;
        buf << f.rdbuf();
        return buf.str();
    };

    std::string logDir = base + "/logs";
    if (fs::exists(logDir)) {
        std::string latestLog;
        std::int64_t latestTime = 0;
        for (const auto& entry : fs::directory_iterator(logDir)) {
            if (entry.is_regular_file()) {
                auto mtime = fs::last_write_time(entry);
                auto epoch = static_cast<int64_t>(
#if __cplusplus >= 202002L
                    std::chrono::duration_cast<std::chrono::seconds>(mtime.time_since_epoch()).count()
#else
                    0
#endif
                );
                if (epoch >= latestTime) {
                    latestTime = epoch;
                    latestLog = entry.path().string();
                }
            }
        }
        if (!latestLog.empty()) {
            report.crashLog = readFileIfExists(latestLog);
        }
    }

    report.importLog = readFileIfExists(base + "/logs/imports.log");
    report.metalLog = readFileIfExists(base + "/logs/metal.log");

    return report;
}

std::vector<CrashReport> CrashReporter::getRecentReports(int count) {
    std::vector<CrashReport> reports;
    std::string dir = getReportsDir();
    if (!fs::exists(dir))
        return reports;

    try {
        std::vector<fs::path> files;
        for (const auto& entry : fs::directory_iterator(dir)) {
            if (entry.is_regular_file() && entry.path().extension() == ".crash") {
                files.push_back(entry.path());
            }
        }

        std::sort(files.begin(), files.end(),
                  [](const fs::path& a, const fs::path& b) { return fs::last_write_time(a) > fs::last_write_time(b); });

        int loaded = 0;
        for (const auto& f : files) {
            if (loaded >= count)
                break;

            std::ifstream file(f);
            if (!file.is_open())
                continue;

            CrashReport report;
            report.id = f.stem().string();
            std::string line;
            while (std::getline(file, line)) {
                if (line.find("timestamp: ") == 0)
                    report.timestamp = line.substr(11);
                else if (line.find("game: ") == 0)
                    report.gameName = line.substr(6);
                else if (line.find("exe: ") == 0)
                    report.exePath = line.substr(5);
                else if (line.find("exit_code: ") == 0)
                    report.exitCode = std::stoi(line.substr(11));
                else if (line.find("fault_address: ") == 0)
                    report.faultAddress = std::stoull(line.substr(15), nullptr, 16);
                else if (line == "--- system ---") {
                    std::ostringstream sys;
                    while (std::getline(file, line) && line != "--- end ---") {
                        sys << line << "\n";
                    }
                    report.systemInfo = sys.str();
                }
            }
            report.collected = true;
            reports.push_back(report);
            loaded++;
        }
    } catch (...) {
    }

    return reports;
}

bool CrashReporter::saveReport(const CrashReport& report) {
    const fs::path base = dataDirectory();
    const fs::path dir = base / "crashes";
    if (!runtime_detail::ensurePrivateDirectory(base) || !runtime_detail::ensurePrivateDirectory(dir))
        return false;

    const fs::path path = dir / (report.id + ".crash");
    std::ofstream f;
    if (!runtime_detail::openPrivateFile(f, path))
        return false;

    f << "id: " << report.id << "\n";
    f << "timestamp: " << report.timestamp << "\n";
    f << "game: " << report.gameName << "\n";
    f << "exe: " << report.exePath << "\n";
    f << "exit_code: " << report.exitCode << "\n";
    f << "fault_address: 0x" << std::hex << report.faultAddress << std::dec << "\n";
    f << "--- system ---\n";
    f << report.systemInfo;
    f << "--- end ---\n";

    if (!report.crashLog.empty()) {
        f << "--- crash_log ---\n";
        f << report.crashLog << "\n";
        f << "--- end ---\n";
    }

    if (!report.importLog.empty()) {
        f << "--- import_log ---\n";
        f << report.importLog << "\n";
        f << "--- end ---\n";
    }

    return true;
}

bool CrashReporter::deleteReport(const std::string& id) {
    std::string path = getReportsDir() + "/" + id + ".crash";
    return fs::remove(path);
}

} // namespace metalsharp
