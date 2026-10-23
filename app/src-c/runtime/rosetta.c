#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE
#endif
#include "metalsharp_backend/rosetta.h"
#include "metalsharp_backend/json.h"
#include "metalsharp_backend/logs.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __APPLE__
#include <sys/sysctl.h>
#endif

/* Upgrades to macOS 27 have been seen to leave Rosetta 2 uninstalled. */
#define MS_ROSETTA_REINSTALL_FROM_MAJOR 27

/* The host's macOS major version, or METALSHARP_ROSETTA_HOST_MACOS's for tests; 0 if unknown.
 * sw_vers rather than a sysctl, since older-SDK processes can be shown a compatibility version. */
static int host_macos_major(void) {
    const char* override = getenv("METALSHARP_ROSETTA_HOST_MACOS");
    char version[32] = "";
    FILE* pipe;
    if (override && override[0])
        return atoi(override);
    pipe = popen("/usr/bin/sw_vers -productVersion", "r");
    if (!pipe)
        return 0;
    if (!fgets(version, sizeof(version), pipe))
        version[0] = '\0';
    if (pclose(pipe) != 0)
        return 0;
    return atoi(version);
}

/* Rosetta exists only on Apple Silicon; METALSHARP_ROSETTA_HOST_ARM64 overrides for tests. */
static bool host_is_arm64(void) {
    const char* override = getenv("METALSHARP_ROSETTA_HOST_ARM64");
    if (override && override[0])
        return !strcmp(override, "1");
#ifdef __APPLE__
    int arm64 = 0;
    size_t size = sizeof(arm64);
    return sysctlbyname("hw.optional.arm64", &arm64, &size, NULL, 0) == 0 && arm64 == 1;
#else
    return false;
#endif
}

static char* home_path(const char* home, const char* relative) {
    size_t length = strlen(home) + strlen(relative) + 2;
    char* path = malloc(length);
    if (path)
        snprintf(path, length, "%s/%s", home, relative);
    return path;
}

static char* read_small_file(const char* path) {
    FILE* file = fopen(path, "rb");
    char* text;
    size_t got;
    if (!file)
        return NULL;
    text = calloc(1, 65537);
    got = text ? fread(text, 1, 65536, file) : 0;
    fclose(file);
    if (text)
        text[got] = '\0';
    return text;
}

static int recorded_major(const char* home) {
    char* path = home_path(home, "config/rosetta-macos");
    char* text = path ? read_small_file(path) : NULL;
    int major = text ? atoi(text) : 0;
    free(text);
    free(path);
    return major;
}

static void record_major(const char* home, int major) {
    char *dir = home_path(home, "config"), *path = home_path(home, "config/rosetta-macos");
    FILE* file;
    if (dir && path && (mkdir(dir, 0755) == 0 || errno == EEXIST) && (file = fopen(path, "w"))) {
        fprintf(file, "%d\n", major);
        fclose(file);
    }
    free(dir);
    free(path);
}

/* Setup installs Rosetta itself, so until it has completed there is nothing to repair. */
static bool setup_completed(const char* home) {
    char* path = home_path(home, "setup.json");
    char* text = path ? read_small_file(path) : NULL;
    char error[64];
    ms_json* setup = text ? ms_json_parse(text, strlen(text), error, sizeof(error)) : NULL;
    bool completed = false;
    if (setup && ms_json_type_of(setup) == MS_JSON_OBJECT)
        (void)ms_json_as_bool(ms_json_object_get(setup, "completed"), &completed);
    ms_json_free(setup);
    free(text);
    free(path);
    return completed;
}

/* softwareupdate --install-rosetta --agree-to-license (METALSHARP_ROSETTA_INSTALLER for tests),
 * its output appended to logs/rosetta.log. */
static bool install_rosetta(const char* home) {
    const char* override = getenv("METALSHARP_ROSETTA_INSTALLER");
    const char* installer = override && override[0] ? override : "/usr/sbin/softwareupdate";
    char *logs = home_path(home, "logs"), *log_path = home_path(home, "logs/rosetta.log");
    int status = 0, log_fd = -1;
    pid_t pid;
    if (logs && (mkdir(logs, 0755) == 0 || errno == EEXIST) && log_path)
        log_fd = open(log_path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    free(logs);
    free(log_path);
    pid = fork();
    if (pid == 0) {
        int null_fd = open("/dev/null", O_RDONLY);
        if (null_fd >= 0)
            dup2(null_fd, STDIN_FILENO);
        if (log_fd >= 0) {
            dup2(log_fd, STDOUT_FILENO);
            dup2(log_fd, STDERR_FILENO);
        }
        execl(installer, "softwareupdate", "--install-rosetta", "--agree-to-license", (char*)NULL);
        _exit(127);
    }
    if (log_fd >= 0)
        close(log_fd);
    if (pid < 0)
        return false;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
        ;
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

bool ms_rosetta_after_macos_upgrade(const char* home) {
    int major, recorded;
    bool installed;
    char message[160];
    if (!home || !host_is_arm64() || (major = host_macos_major()) <= 0)
        return false;
    recorded = recorded_major(home);
    if (recorded == major)
        return false;
    /* Before macOS 27, and before setup has run (it installs Rosetta), only remember the version. */
    if (major < MS_ROSETTA_REINSTALL_FROM_MAJOR || !setup_completed(home)) {
        record_major(home, major);
        return false;
    }
    /* No recorded version means an install from before this check, which may already have been
     * upgraded: reinstall once too. */
    if (recorded > 0)
        snprintf(message, sizeof(message), "macOS %d (last checked on %d): reinstalling Rosetta 2", major, recorded);
    else
        snprintf(message, sizeof(message), "macOS %d, not checked before: reinstalling Rosetta 2", major);
    ms_log_event(home, message);
    installed = install_rosetta(home);
    /* Recorded only on success, so a failed install (offline, say) is retried at the next start. */
    if (installed)
        record_major(home, major);
    snprintf(message, sizeof(message), "Rosetta 2 reinstall after the macOS %d upgrade %s%s", major,
             installed ? "succeeded" : "failed", installed ? "" : "; see logs/rosetta.log");
    ms_log_event(home, message);
    return true;
}

static void* rosetta_worker(void* arg) {
    char* home = arg;
    (void)ms_rosetta_after_macos_upgrade(home);
    free(home);
    return NULL;
}

void ms_rosetta_after_macos_upgrade_async(const char* home) {
    pthread_t thread;
    char* copy = home ? strdup(home) : NULL;
    if (!copy)
        return;
    if (pthread_create(&thread, NULL, rosetta_worker, copy) != 0) {
        free(copy);
        return;
    }
    (void)pthread_detach(thread);
}
