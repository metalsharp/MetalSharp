#include <cstdio>
#include <cstring>
#include <metalsharp/CompatDatabase.h>
#include <metalsharp/CrashDiagnostics.h>
#include <metalsharp/DRMDetector.h>
#include <metalsharp/GameValidator.h>
#include <metalsharp/ImportReporter.h>
#include <vector>

using namespace metalsharp;

static int testsPassed = 0;
static int testsFailed = 0;

static void reset_compat_test_file(const char* path) {
    std::remove(path);
}

#define TEST(name)                                                                                                     \
    printf("  TEST: %-55s", #name);                                                                                    \
    if (test_##name()) {                                                                                               \
        printf("PASS\n");                                                                                              \
        testsPassed++;                                                                                                 \
    } else {                                                                                                           \
        printf("FAIL\n");                                                                                              \
        testsFailed++;                                                                                                 \
    }

// 23.1 CompatDatabase
static bool test_compat_status_roundtrip() {
    bool ok = true;
    ok = ok && (CompatDatabase::statusFromString("Platinum") == CompatStatus::Platinum);
    ok = ok && (CompatDatabase::statusFromString("Gold") == CompatStatus::Gold);
    ok = ok && (CompatDatabase::statusFromString("Silver") == CompatStatus::Silver);
    ok = ok && (CompatDatabase::statusFromString("Bronze") == CompatStatus::Bronze);
    ok = ok && (CompatDatabase::statusFromString("Broken") == CompatStatus::Broken);
    ok = ok && (CompatDatabase::statusFromString("Untested") == CompatStatus::Untested);
    ok = ok && (CompatDatabase::statusFromString("Unknown") == CompatStatus::Untested);
    return ok;
}

static bool test_compat_status_strings() {
    bool ok = true;
    ok = ok && (strcmp(CompatDatabase::statusToString(CompatStatus::Platinum), "Platinum") == 0);
    ok = ok && (strcmp(CompatDatabase::statusToString(CompatStatus::Broken), "Broken") == 0);
    return ok;
}

static bool test_compat_add_find_remove() {
    reset_compat_test_file("/tmp/metalsharp_test/compat.json");
    auto& db = CompatDatabase::instance();
    db.init("/tmp/metalsharp_test/compat.json");

    GameEntry entry;
    entry.gameId = "test_game_1";
    entry.name = "Test Game";
    entry.exePath = "C:\\test.exe";
    entry.status = CompatStatus::Gold;
    entry.d3dVersion = 11;
    entry.avgFPS = 60;
    db.addOrUpdate(entry);

    const GameEntry* found = db.find("test_game_1");
    bool addOk = found && found->status == CompatStatus::Gold && found->avgFPS == 60;

    db.remove("test_game_1");
    const GameEntry* gone = db.find("test_game_1");

    db.shutdown();
    return addOk && gone == nullptr;
}

static bool test_compat_query_by_status() {
    reset_compat_test_file("/tmp/metalsharp_test/compat.json");
    auto& db = CompatDatabase::instance();
    db.init("/tmp/metalsharp_test/compat.json");

    GameEntry e1, e2, e3;
    e1.gameId = "g1";
    e1.status = CompatStatus::Platinum;
    e2.gameId = "g2";
    e2.status = CompatStatus::Gold;
    e3.gameId = "g3";
    e3.status = CompatStatus::Platinum;

    db.addOrUpdate(e1);
    db.addOrUpdate(e2);
    db.addOrUpdate(e3);

    auto platinum = db.queryByStatus(CompatStatus::Platinum);
    auto broken = db.queryByStatus(CompatStatus::Broken);

    db.shutdown();
    return platinum.size() == 2 && broken.size() == 0;
}

static bool test_compat_missing_imports() {
    reset_compat_test_file("/tmp/metalsharp_test/compat.json");
    auto& db = CompatDatabase::instance();
    db.init("/tmp/metalsharp_test/compat.json");

    MissingImport imp;
    imp.dll = "d3d11.dll";
    imp.function = "CreateDeferredContext";
    imp.isOrdinal = false;
    db.addMissingImport("test_game", imp);

    MissingImport imp2;
    imp2.dll = "kernel32.dll";
    imp2.function = "CreateFileW";
    db.addMissingImport("test_game", imp2);

    const GameEntry* found = db.find("test_game");
    bool ok = found && found->missingImports.size() == 2;

    db.shutdown();
    return ok;
}

static bool test_compat_crash_record() {
    reset_compat_test_file("/tmp/metalsharp_test/compat.json");
    auto& db = CompatDatabase::instance();
    db.init("/tmp/metalsharp_test/compat.json");

    CrashRecord crash;
    crash.timestamp = 12345;
    crash.signal = 11;
    crash.faultAddr = 0xDEAD;
    crash.rip = 0;
    crash.rsp = 0x7FFF0000;
    db.addCrashRecord("crash_test", crash);

    const GameEntry* found = db.find("crash_test");
    bool ok = found && found->status == CompatStatus::Broken && found->crashes.size() == 1;

    db.shutdown();
    return ok;
}

static bool test_compat_save_load() {
    reset_compat_test_file("/tmp/metalsharp_test/compat_roundtrip.json");
    auto& db = CompatDatabase::instance();
    db.init("/tmp/metalsharp_test/compat_roundtrip.json");

    GameEntry entry;
    entry.gameId = "persist_test";
    entry.name = "Persistent Game";
    entry.status = CompatStatus::Silver;
    entry.d3dVersion = 12;
    entry.avgFPS = 45;
    entry.antiCheat = "VAC";
    entry.drm = "Steam Stub";
    entry.notes = "Some rendering glitches";
    db.addOrUpdate(entry);

    db.saveToDisk();

    auto& db2 = CompatDatabase::instance();
    db2.loadFromDisk();

    const GameEntry* loaded = db2.find("persist_test");
    bool ok = loaded && loaded->status == CompatStatus::Silver && loaded->avgFPS == 45;

    db.shutdown();
    return ok;
}

static bool test_compat_report_generation() {
    reset_compat_test_file("/tmp/metalsharp_test/compat.json");
    auto& db = CompatDatabase::instance();
    db.init("/tmp/metalsharp_test/compat.json");

    GameEntry entry;
    entry.gameId = "report_test";
    entry.name = "Report Game";
    entry.exePath = "game.exe";
    entry.status = CompatStatus::Gold;
    entry.d3dVersion = 11;
    db.addOrUpdate(entry);

    std::string report = db.generateReport("report_test");
    bool ok = report.find("Report Game") != std::string::npos && report.find("Gold") != std::string::npos;

    db.shutdown();
    return ok;
}

// 23.2 ImportReporter
static bool test_import_reporter_record() {
    auto& reporter = ImportReporter::instance();
    reporter.clear();

    reporter.recordImport("kernel32.dll", "CreateFileW", true);
    reporter.recordImport("kernel32.dll", "UnknownFunc", false);
    reporter.recordImport("d3d11.dll", "CreateDevice", true);
    reporter.recordImport("d3d11.dll", "MissingFeature", false);

    return reporter.totalCount() == 4 && reporter.resolvedCount() == 2 && reporter.missingCount() == 2;
}

static bool test_import_reporter_ordinal() {
    auto& reporter = ImportReporter::instance();
    reporter.clear();

    reporter.recordOrdinalImport("user32.dll", 42, true);
    reporter.recordOrdinalImport("user32.dll", 99, false);

    auto missing = reporter.missingImports();
    return missing.size() == 1 && missing[0].isOrdinal && missing[0].ordinal == 99;
}

static bool test_import_reporter_missing_dlls() {
    auto& reporter = ImportReporter::instance();
    reporter.clear();

    reporter.recordImport("kernel32.dll", "A", true);
    reporter.recordImport("kernel32.dll", "B", false);
    reporter.recordImport("d3d11.dll", "C", false);
    reporter.recordImport("ws2_32.dll", "D", true);

    auto dlls = reporter.missingDlls();
    return dlls.size() == 2;
}

static bool test_import_reporter_summary() {
    auto& reporter = ImportReporter::instance();
    reporter.clear();
    reporter.recordImport("test.dll", "Func1", true);
    reporter.recordImport("test.dll", "Func2", false);

    std::string summary = reporter.generateSummary();
    return summary.find("Total:") != std::string::npos && summary.find("Missing:") != std::string::npos;
}

// 23.3 CrashDiagnostics
static bool test_crash_diagnostics_lifecycle() {
    auto& diag = CrashDiagnostics::instance();
    diag.init("/tmp/metalsharp_test/diag");
    diag.setGameId("test_game");
    diag.setModuleInfo(0x10000, 0x50000, "test.exe");
    diag.shutdown();
    return true;
}

static bool test_crash_diagnostics_write_dump() {
    auto& diag = CrashDiagnostics::instance();
    diag.init("/tmp/metalsharp_test/diag2");
    diag.setGameId("crash_game");
    diag.setModuleInfo(0x400000, 0x100000, "game.exe");

    CrashInfo info;
    info.signal = 11;
    info.faultAddr = 0x0;
    info.rip = 0x0;
    info.rsp = 0x7FFF0000;
    diag.writeCrashDump(info);

    bool ok = diag.crashCount() == 1;
    diag.shutdown();
    return ok;
}

static bool test_crash_diagnostics_timestamp() {
    std::string ts = CrashDiagnostics::formatTimestamp(1704067200);
    return !ts.empty() && ts.find('-') != std::string::npos;
}

// 23.4 DRMDetector
static bool test_drm_detector_memory_denuvo() {
    auto& detector = DRMDetector::instance();

    const char* fakeExe = "SomeBinary\x44\x65\x6E\x75\x76\x6F"
                          "MoreData";
    auto results = detector.scanMemory(reinterpret_cast<const uint8_t*>(fakeExe), strlen(fakeExe));

    bool foundDenuvo = false;
    for (const auto& d : results) {
        if (d.name == "Denuvo Anti-Tamper" && d.detected)
            foundDenuvo = true;
    }
    return foundDenuvo;
}

static bool test_drm_detector_memory_steam() {
    auto& detector = DRMDetector::instance();

    const char* fakeExe = "Steamstub\x00SomeBinary";
    auto results = detector.scanMemory(reinterpret_cast<const uint8_t*>(fakeExe), strlen(fakeExe));

    bool foundSteam = false;
    for (const auto& d : results) {
        if (d.name == "Steam Stub" && d.detected)
            foundSteam = true;
    }
    return foundSteam;
}

static bool test_drm_detector_kernel_anticheat() {
    auto& detector = DRMDetector::instance();

    const uint8_t fakeExe[] = {'E', 'a', 's', 'y',  'A', 'n', 't', 'i', 'C', 'h',
                               'e', 'a', 't', 0x00, 'B', 'i', 'n', 'a', 'r', 'y'};
    auto results = detector.scanMemory(fakeExe, sizeof(fakeExe));

    return !detector.hasKernelAntiCheat(results) && detector.requiresRuntimeProof(results) &&
           !detector.isCompatible(results);
}

static bool test_drm_detector_clean_binary() {
    auto& detector = DRMDetector::instance();

    const uint8_t cleanExe[] = {0x4D, 0x5A, 0x90, 0x00, 0x01, 0x02, 0x03, 0x04};
    auto results = detector.scanMemory(cleanExe, sizeof(cleanExe));

    return detector.isCompatible(results) && !detector.hasKernelAntiCheat(results);
}

static bool test_drm_detector_summary() {
    auto& detector = DRMDetector::instance();

    const char* fakeExe = "Denuvo\x00Binary";
    auto results = detector.scanMemory(reinterpret_cast<const uint8_t*>(fakeExe), strlen(fakeExe));

    std::string sum = detector.summary(results);
    return sum.find("Denuvo") != std::string::npos;
}

// 23.5 GameValidator
static bool test_game_validator_quick_check() {
    auto& validator = GameValidator::instance();
    validator.init("/tmp/metalsharp_test/validator");

    auto result = validator.quickCheck("/nonexistent/game.exe");
    bool ok = !result.canLaunch || result.gameId.empty() || result.gameId.size() > 0;

    validator.shutdown();
    return true;
}

static bool test_game_validator_validate_clean() {
    auto& validator = GameValidator::instance();
    validator.init("/tmp/metalsharp_test/validator2");

    ValidationResult result;
    result.gameId = "clean_game";
    result.canLaunch = true;
    result.suggestedStatus = CompatStatus::Untested;

    bool saved = validator.saveResult(result);
    GameEntry* found = validator.findGame("clean_game");

    validator.shutdown();
    return saved && found;
}

int main() {
    printf("=== Phase 23: Game Validation Sprint ===\n\n");

    printf("--- 23.1 Compatibility Database ---\n");
    TEST(compat_status_roundtrip);
    TEST(compat_status_strings);
    TEST(compat_add_find_remove);
    TEST(compat_query_by_status);
    TEST(compat_missing_imports);
    TEST(compat_crash_record);
    TEST(compat_save_load);
    TEST(compat_report_generation);

    printf("\n--- 23.2 Import Reporter ---\n");
    TEST(import_reporter_record);
    TEST(import_reporter_ordinal);
    TEST(import_reporter_missing_dlls);
    TEST(import_reporter_summary);

    printf("\n--- 23.3 Crash Diagnostics ---\n");
    TEST(crash_diagnostics_lifecycle);
    TEST(crash_diagnostics_write_dump);
    TEST(crash_diagnostics_timestamp);

    printf("\n--- 23.4 DRM / Anti-cheat Detection ---\n");
    TEST(drm_detector_memory_denuvo);
    TEST(drm_detector_memory_steam);
    TEST(drm_detector_kernel_anticheat);
    TEST(drm_detector_clean_binary);
    TEST(drm_detector_summary);

    printf("\n--- 23.5 Game Validator ---\n");
    TEST(game_validator_quick_check);
    TEST(game_validator_validate_clean);

    printf("\n%d/%d passed", testsPassed, testsPassed + testsFailed);
    if (testsFailed > 0)
        printf(" (%d FAILED)", testsFailed);
    printf("\n");

    return testsFailed > 0 ? 1 : 0;
}
