#include "metalsharp/CrashDiagnostics.h"
#include "metalsharp/CrashReporter.h"
#include "metalsharp/GameDetector.h"
#include "metalsharp/SettingsManager.h"
#include "metalsharp/UpdateChecker.h"
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

static int g_pass = 0;
static int g_fail = 0;

#define TEST(name)                                                                                                     \
    do {                                                                                                               \
        printf("  TEST: %-50s", #name);                                                                                \
        fflush(stdout);                                                                                                \
        if (name()) {                                                                                                  \
            printf("PASS\n");                                                                                          \
            g_pass++;                                                                                                  \
        } else {                                                                                                       \
            printf("FAIL\n");                                                                                          \
            g_fail++;                                                                                                  \
        }                                                                                                              \
    } while (0)

static bool game_detector_platform_strings() {
    using namespace metalsharp;
    assert(GameDetector::platformToString(GamePlatform::Steam) == "steam");
    assert(GameDetector::platformToString(GamePlatform::EpicGamesStore) == "epic");
    assert(GameDetector::platformToString(GamePlatform::GOG) == "gog");
    assert(GameDetector::platformToString(GamePlatform::Local) == "local");
    assert(GameDetector::platformToString(GamePlatform::Unknown) == "unknown");
    return true;
}

static bool game_detector_platform_from_string() {
    using namespace metalsharp;
    assert(GameDetector::platformFromString("steam") == GamePlatform::Steam);
    assert(GameDetector::platformFromString("epic") == GamePlatform::EpicGamesStore);
    assert(GameDetector::platformFromString("gog") == GamePlatform::GOG);
    assert(GameDetector::platformFromString("local") == GamePlatform::Local);
    assert(GameDetector::platformFromString("other") == GamePlatform::Unknown);
    return true;
}

static bool game_detector_detect_local() {
    auto games = metalsharp::GameDetector::detectLocal();
    return true;
}

static bool game_detector_detect_all() {
    auto games = metalsharp::GameDetector::detectAll();
    return true;
}

static bool game_detector_local_scan() {
    const char* home = std::getenv("HOME");
    if (!home)
        return true;

    std::string testDir = std::string(home) + "/.metalsharp/games";
    if (!fs::exists(testDir))
        return true;

    auto games = metalsharp::GameDetector::detectLocal();
    for (const auto& g : games) {
        assert(g.platform == metalsharp::GamePlatform::Local);
        assert(!g.id.empty());
        assert(g.id.find("local_") == 0);
    }
    return true;
}

static bool crash_reporter_lifecycle() {
    auto& cr = metalsharp::CrashReporter::instance();
    cr.beginSession("TestGame", "/path/to/game.exe");
    cr.endSession(0);
    return true;
}

static bool crash_reporter_generate_id() {
    std::string id1 = metalsharp::CrashReporter::generateId();
    std::string id2 = metalsharp::CrashReporter::generateId();
    assert(!id1.empty());
    assert(!id2.empty());
    assert(id1 != id2);
    return true;
}

static bool crash_reporter_format_timestamp() {
    std::string ts = metalsharp::CrashReporter::formatTimestamp(0);
    assert(!ts.empty());
    assert(metalsharp::CrashReporter::formatTimestamp(std::numeric_limits<int64_t>::max()).empty());
    return true;
}

static bool crash_reporter_collect_system_info() {
    auto& cr = metalsharp::CrashReporter::instance();
    std::string info = cr.collectSystemInfo();
    assert(!info.empty());
    assert(info.find("Platform:") != std::string::npos);
    assert(info.find("Architecture:") != std::string::npos);
    assert(info.find("Home: [redacted]") != std::string::npos);
    const char* home = std::getenv("HOME");
    if (home && *home)
        assert(info.find(home) == std::string::npos);
    return true;
}

static bool private_mode(const fs::path& path, mode_t expected) {
    struct stat st{};
    return stat(path.c_str(), &st) == 0 && (st.st_mode & 0777) == expected;
}

static bool crash_reporter_private_storage() {
    auto& cr = metalsharp::CrashReporter::instance();
    const char* previousHome = std::getenv("HOME");
    const bool hadHome = previousHome != nullptr;
    const std::string previousHomeValue = previousHome ? previousHome : "";
    const fs::path tempHome = fs::temp_directory_path() / ("metalsharp-crash-reporter-" + std::to_string(getpid()));
    fs::remove_all(tempHome);

    if (setenv("HOME", tempHome.c_str(), 1) != 0)
        return false;

    metalsharp::CrashReport report;
    report.id = "private-storage";
    report.systemInfo = "test\n";
    const bool saved = cr.saveReport(report);
    const fs::path dataDir = tempHome / ".metalsharp";
    const fs::path reportsDir = dataDir / "crashes";
    const fs::path reportPath = reportsDir / "private-storage.crash";
    const bool privateStorage =
        saved && private_mode(dataDir, 0700) && private_mode(reportsDir, 0700) && private_mode(reportPath, 0600);

    cr.deleteReport(report.id);
    fs::remove_all(tempHome);
    if (hadHome)
        setenv("HOME", previousHomeValue.c_str(), 1);
    else
        unsetenv("HOME");
    return privateStorage;
}

static bool crash_reporter_home_fallback() {
    auto& cr = metalsharp::CrashReporter::instance();
    const char* previousHome = std::getenv("HOME");
    const bool hadHome = previousHome != nullptr;
    const std::string previousHomeValue = previousHome ? previousHome : "";

    unsetenv("HOME");
    const std::string reportsDir = cr.getReportsDir();
    if (hadHome)
        setenv("HOME", previousHomeValue.c_str(), 1);
    else
        unsetenv("HOME");

    return reportsDir.find("/tmp/") == std::string::npos && reportsDir.ends_with("/crashes");
}

static bool crash_diagnostics_private_storage() {
    const fs::path diagDir = fs::temp_directory_path() / ("metalsharp-crash-diagnostics-" + std::to_string(getpid()));
    fs::remove_all(diagDir);

    auto& diagnostics = metalsharp::CrashDiagnostics::instance();
    diagnostics.init(diagDir.string());
    const bool privateDirectories = private_mode(diagDir, 0700) && private_mode(diagDir / "crashes", 0700);

    if (!privateDirectories) {
        diagnostics.shutdown();
        fs::remove_all(diagDir);
        return false;
    }

    metalsharp::CrashInfo info;
    info.signal = 11;
    diagnostics.setGameId("test-game");
    diagnostics.setModuleInfo(0x1000, 0x1000, "/path/to/game.exe");
    diagnostics.writeCrashDump(info);
    diagnostics.writeDiagnosticsBundle();

    bool privateFiles = private_mode(diagDir / "diagnostics.txt", 0600);
    bool foundCrashDump = false;
    for (const auto& entry : fs::directory_iterator(diagDir / "crashes")) {
        if (entry.path().extension() == ".txt") {
            foundCrashDump = true;
            privateFiles = privateFiles && private_mode(entry.path(), 0600);
        }
    }

    diagnostics.shutdown();
    fs::remove_all(diagDir);
    return privateDirectories && privateFiles && foundCrashDump;
}

static bool crash_diagnostics_format_timestamp() {
    assert(!metalsharp::CrashDiagnostics::formatTimestamp(0).empty());
    assert(metalsharp::CrashDiagnostics::formatTimestamp(std::numeric_limits<uint64_t>::max() / 2).empty());
    return true;
}

static bool crash_reporter_save_and_load() {
    auto& cr = metalsharp::CrashReporter::instance();
    cr.beginSession("TestGame2", "/path/to/game2.exe");

    auto report = cr.collectReport();
    assert(!report.id.empty());
    assert(report.collected);
    assert(report.gameName == "TestGame2");
    assert(report.exePath == "/path/to/game2.exe");

    bool saved = cr.saveReport(report);
    assert(saved);

    auto reports = cr.getRecentReports(10);
    bool found = false;
    for (const auto& r : reports) {
        if (r.id == report.id) {
            found = true;
            assert(r.gameName == "TestGame2");
            break;
        }
    }

    if (found) {
        bool deleted = cr.deleteReport(report.id);
        assert(deleted);
    }

    cr.endSession(0);
    return true;
}

static bool crash_reporter_reports_dir() {
    auto& cr = metalsharp::CrashReporter::instance();
    std::string dir = cr.getReportsDir();
    assert(!dir.empty());
    assert(dir.find("metalsharp") != std::string::npos);
    return true;
}

static bool version_parse() {
    auto v = metalsharp::Version::parse("1.2.3");
    assert(v.major == 1);
    assert(v.minor == 2);
    assert(v.patch == 3);
    assert(v.prerelease.empty());

    auto v2 = metalsharp::Version::parse("v2.0.1-beta");
    assert(v2.major == 2);
    assert(v2.minor == 0);
    assert(v2.patch == 1);
    assert(v2.prerelease == "beta");

    auto v3 = metalsharp::Version::parse("0.1.0");
    assert(v3.major == 0);
    assert(v3.minor == 1);
    assert(v3.patch == 0);
    return true;
}

static bool version_comparison() {
    auto v1 = metalsharp::Version::parse("1.0.0");
    auto v2 = metalsharp::Version::parse("2.0.0");
    auto v3 = metalsharp::Version::parse("1.1.0");
    auto v4 = metalsharp::Version::parse("1.0.1");
    auto v5 = metalsharp::Version::parse("1.0.0");

    assert(v2 > v1);
    assert(v3 > v1);
    assert(v4 > v1);
    assert(v1 == v5);
    assert(v1 >= v5);
    assert(v2 >= v1);
    assert(!(v1 > v2));
    return true;
}

static bool version_prerelease_comparison() {
    auto stable = metalsharp::Version::parse("1.0.1");
    auto releaseCandidate = metalsharp::Version::parse("1.0.1-rc1");
    assert(stable > releaseCandidate);
    assert(!(releaseCandidate > stable));
    assert(!(stable == releaseCandidate));

    assert(metalsharp::Version::parse("1.0.0-alpha.1") > metalsharp::Version::parse("1.0.0-alpha"));
    assert(metalsharp::Version::parse("1.0.0-alpha.beta") > metalsharp::Version::parse("1.0.0-alpha.1"));
    assert(metalsharp::Version::parse("1.0.0-beta") > metalsharp::Version::parse("1.0.0-alpha.beta"));
    assert(metalsharp::Version::parse("1.0.0-beta.2") > metalsharp::Version::parse("1.0.0-beta.1"));
    assert(metalsharp::Version::parse("1.0.0-beta.11") > metalsharp::Version::parse("1.0.0-beta.2"));
    assert(metalsharp::Version::parse("1.0.0-rc.1") > metalsharp::Version::parse("1.0.0-beta.11"));
    assert(stable > metalsharp::Version::parse("1.0.1-rc.1"));

    assert(metalsharp::Version::parse("1.0.0+build.1") == metalsharp::Version::parse("1.0.0"));
    assert(metalsharp::Version::parse("1.0.0-alpha+build.1") == metalsharp::Version::parse("1.0.0-alpha"));
    return true;
}

static bool version_to_string() {
    auto v = metalsharp::Version::parse("1.2.3");
    assert(v.toString() == "1.2.3");

    auto v2 = metalsharp::Version::parse("2.0.0-alpha");
    assert(v2.toString() == "2.0.0-alpha");
    return true;
}

static bool update_checker_current_version() {
    auto v = metalsharp::UpdateChecker::getCurrentVersion();
    assert(v.major == 0);
    assert(v.minor == 1);
    assert(v.patch == 0);
    return true;
}

static bool update_checker_user_agent() {
    std::string ua = metalsharp::UpdateChecker::getUserAgent();
    assert(!ua.empty());
    assert(ua.find("MetalSharp") != std::string::npos);
    return true;
}

static bool update_checker_parse_release() {
    std::string json =
        R"({"tag_name":"v0.2.0","html_url":"https://github.com/test/repo/releases/tag/v0.2.0","zipball_url":"https://github.com/test/repo/zipball/v0.2.0","body":"Release notes with a \"quoted\" line\n"})";

    auto current = metalsharp::Version::parse("0.1.0");
    auto info = metalsharp::UpdateChecker::parseGitHubRelease(json, current);

    assert(info.available);
    assert(info.latestVersion.major == 0);
    assert(info.latestVersion.minor == 2);
    assert(info.latestVersion.patch == 0);
    assert(info.currentVersion == "0.1.0");
    assert(info.releaseNotes == "Release notes with a \"quoted\" line\n");
    assert(!info.downloadUrl.empty());
    assert(!info.htmlUrl.empty());
    return true;
}

static bool update_checker_no_update() {
    std::string json =
        R"({"tag_name":"v0.1.0","html_url":"https://github.com/test/repo","zipball_url":"https://github.com/test/repo/zipball","body":""})";

    auto current = metalsharp::Version::parse("0.1.0");
    auto info = metalsharp::UpdateChecker::parseGitHubRelease(json, current);

    assert(!info.available);
    assert(info.latestVersion == current);
    return true;
}

static bool update_checker_rejects_invalid_repo() {
    auto info = metalsharp::UpdateChecker::checkForUpdates("not a repository");
    assert(!info.available);
    assert(info.currentVersion == metalsharp::UpdateChecker::getCurrentVersion().toString());
    return true;
}

static bool settings_manager_defaults() {
    auto& sm = metalsharp::SettingsManager::instance();
    const auto s = sm.state();
    assert(s.render.renderWidth == 1920);
    assert(s.render.renderHeight == 1080);
    assert(s.render.windowMode == metalsharp::WindowMode::Fullscreen);
    assert(s.render.upscaling == metalsharp::UpscalingQuality::Off);
    assert(s.render.vsync == true);
    assert(s.render.shaderCacheEnabled == true);
    assert(s.render.pipelineCacheEnabled == true);
    assert(s.launchMode == "native");
    return true;
}

static bool settings_manager_string_conversions() {
    using namespace metalsharp;

    assert(SettingsManager::upscalingToString(UpscalingQuality::Off) == "off");
    assert(SettingsManager::upscalingToString(UpscalingQuality::Low) == "low");
    assert(SettingsManager::upscalingToString(UpscalingQuality::Medium) == "medium");
    assert(SettingsManager::upscalingToString(UpscalingQuality::High) == "high");
    assert(SettingsManager::upscalingToString(UpscalingQuality::Ultra) == "ultra");

    assert(SettingsManager::upscalingFromString("off") == UpscalingQuality::Off);
    assert(SettingsManager::upscalingFromString("low") == UpscalingQuality::Low);
    assert(SettingsManager::upscalingFromString("medium") == UpscalingQuality::Medium);
    assert(SettingsManager::upscalingFromString("high") == UpscalingQuality::High);
    assert(SettingsManager::upscalingFromString("ultra") == UpscalingQuality::Ultra);
    assert(SettingsManager::upscalingFromString("unknown") == UpscalingQuality::Off);

    assert(SettingsManager::windowModeToString(WindowMode::Windowed) == "windowed");
    assert(SettingsManager::windowModeToString(WindowMode::Borderless) == "borderless");
    assert(SettingsManager::windowModeToString(WindowMode::Fullscreen) == "fullscreen");

    assert(SettingsManager::windowModeFromString("windowed") == WindowMode::Windowed);
    assert(SettingsManager::windowModeFromString("borderless") == WindowMode::Borderless);
    assert(SettingsManager::windowModeFromString("fullscreen") == WindowMode::Fullscreen);
    assert(SettingsManager::windowModeFromString("unknown") == WindowMode::Fullscreen);
    return true;
}

static bool settings_manager_save_load() {
    auto& sm = metalsharp::SettingsManager::instance();
    auto s = sm.state();
    s.render.renderWidth = 2560;
    s.render.renderHeight = 1440;
    s.render.windowMode = metalsharp::WindowMode::Borderless;
    s.render.upscaling = metalsharp::UpscalingQuality::High;
    s.render.vsync = false;
    s.launchMode = "wine";
    sm.setState(s);

    const auto testPath = fs::temp_directory_path() / "metalsharp_test_settings.json";
    bool saved = sm.save(testPath.string());
    assert(saved);
    const bool fileExists = fs::exists(testPath);
    if (!saved || !fileExists) {
        fs::remove(testPath);
        sm.setState({});
        return false;
    }

    bool loaded = sm.load(testPath.string());
    assert(loaded);
    const auto loadedState = sm.state();
    const bool valuesMatch = loadedState.render.renderWidth == 2560 && loadedState.render.renderHeight == 1440 &&
                             loadedState.render.windowMode == metalsharp::WindowMode::Borderless &&
                             loadedState.render.upscaling == metalsharp::UpscalingQuality::High &&
                             !loadedState.render.vsync && loadedState.launchMode == "wine";

    fs::remove(testPath);

    sm.setState({});
    return loaded && valuesMatch;
}

static bool settings_manager_concurrent_access() {
    auto& sm = metalsharp::SettingsManager::instance();
    const auto uniqueSuffix = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto testDir = fs::temp_directory_path() / ("metalsharp_settings_" + std::to_string(uniqueSuffix));
    const auto testPath = testDir / "settings.json";

    auto initial = sm.state();
    initial.render.renderWidth = 1920;
    initial.render.renderHeight = 1080;
    sm.setState(initial);
    if (!sm.save(testPath.string())) {
        fs::remove_all(testDir);
        return false;
    }

    std::atomic<bool> failed{false};
    std::vector<std::thread> workers;
    for (int worker = 0; worker < 4; ++worker) {
        workers.emplace_back([&, worker] {
            for (int iteration = 0; iteration < 25 && !failed.load(); ++iteration) {
                if (worker == 0) {
                    if (!sm.load(testPath.string())) {
                        failed.store(true);
                        return;
                    }
                    continue;
                }

                auto snapshot = sm.state();
                snapshot.render.renderWidth = 1280 + static_cast<uint32_t>(worker * 100 + iteration);
                snapshot.render.renderHeight = 720 + static_cast<uint32_t>(iteration);
                snapshot.launchMode = "worker-" + std::to_string(worker);
                sm.setState(std::move(snapshot));
                if (!sm.save(testPath.string())) {
                    failed.store(true);
                    return;
                }
                if (iteration % 5 == 0 && !sm.load(testPath.string())) {
                    failed.store(true);
                    return;
                }
            }
        });
    }

    for (auto& worker : workers)
        worker.join();

    std::ifstream savedFile(testPath);
    std::string savedJson((std::istreambuf_iterator<char>(savedFile)), std::istreambuf_iterator<char>());
    const bool validFile = !savedJson.empty() && savedJson.front() == '{' && savedJson.back() == '\n';
    savedFile.close();
    const auto finalState = sm.state();
    fs::remove_all(testDir);
    sm.setState({});
    return !failed.load() && validFile && finalState.render.renderWidth >= 1280;
}

static bool settings_manager_default_path() {
    std::string path = metalsharp::SettingsManager::defaultSettingsPath();
    assert(!path.empty());
    assert(path.find("metalsharp") != std::string::npos);
    assert(path.find("settings.json") != std::string::npos);
    return true;
}

int main() {
    printf("\n=== Phase 24: Polish & 1.0 Release ===\n\n");

    printf("--- 24.1 Game Detector ---\n");
    TEST(game_detector_platform_strings);
    TEST(game_detector_platform_from_string);
    TEST(game_detector_detect_local);
    TEST(game_detector_detect_all);
    TEST(game_detector_local_scan);

    printf("\n--- 24.2 Crash Reporter ---\n");
    TEST(crash_reporter_lifecycle);
    TEST(crash_reporter_generate_id);
    TEST(crash_reporter_format_timestamp);
    TEST(crash_reporter_collect_system_info);
    TEST(crash_reporter_private_storage);
    TEST(crash_reporter_home_fallback);
    TEST(crash_reporter_save_and_load);
    TEST(crash_reporter_reports_dir);

    printf("\n--- 24.2a Crash Diagnostics ---\n");
    TEST(crash_diagnostics_private_storage);
    TEST(crash_diagnostics_format_timestamp);

    printf("\n--- 24.3 Update Checker ---\n");
    TEST(version_parse);
    TEST(version_comparison);
    TEST(version_prerelease_comparison);
    TEST(version_to_string);
    TEST(update_checker_current_version);
    TEST(update_checker_user_agent);
    TEST(update_checker_parse_release);
    TEST(update_checker_no_update);
    TEST(update_checker_rejects_invalid_repo);

    printf("\n--- 24.4 Settings Manager ---\n");
    TEST(settings_manager_defaults);
    TEST(settings_manager_string_conversions);
    TEST(settings_manager_save_load);
    TEST(settings_manager_concurrent_access);
    TEST(settings_manager_default_path);

    printf("\n%d/%d passed (%d FAILED)\n\n", g_pass, g_pass + g_fail, g_fail);
    return g_fail > 0 ? 1 : 0;
}
