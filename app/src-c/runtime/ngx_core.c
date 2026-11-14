#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE
#endif
#include "metalsharp_backend/ngx_core.h"

#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* NVIDIA's NGX code in a game (DLSS through Streamline, which Assassin's Creed Black Flag Resynced uses) finds
 * the NGX core through the registry: HKLM\SOFTWARE\NVIDIA Corporation\Global\NGXCore\FullPath, and the driver's
 * HKLM\SYSTEM\CurrentControlSet\Services\nvlddmkm\NGXCore\NGXPath. Without them it reports no DLSS.
 *
 * On Mullion, Mullion's DXGI writes both, pointing at its own core, whenever NVEXT is on. On D3DMetal nothing does:
 * D3DMetal's DXGI leaves them out, and a Streamline game offers no DLSS (or Frame Generation) although D3DMetal
 * shows it an NVIDIA GPU and its nvapi64 answers as NVIDIA's. On that lane MetalSharp Wine's loader gives a program
 * Apple's nvngx-on-metalfx.dll for System32's nvngx.dll, so a D3DMetal launch points both at System32.
 *
 * Mullion's values must not be left pointing there, so the next launch on another lane takes MetalSharp's values
 * back out: Mullion's DXGI then writes its own again. A marker in the prefix says MetalSharp set them, so a prefix
 * is only changed when its lane changes. */

static const char* const NGX_KEYS[][2] = {
    {"HKLM\\SOFTWARE\\NVIDIA Corporation\\Global\\NGXCore", "FullPath"},
    {"HKLM\\SYSTEM\\CurrentControlSet\\Services\\nvlddmkm\\NGXCore", "NGXPath"},
};

static bool marker_path(const char* prefix, char* out, size_t size) {
    return prefix && prefix[0] && (size_t)snprintf(out, size, "%s/%s", prefix, MS_NGX_CORE_MARKER) < size;
}

ms_ngx_core_action ms_ngx_core_action_for(const char* prefix, bool d3dmetal) {
    char marker[PATH_MAX];
    bool set;
    if (!marker_path(prefix, marker, sizeof(marker)))
        return MS_NGX_CORE_KEEP;
    set = access(marker, F_OK) == 0;
    if (d3dmetal && !set)
        return MS_NGX_CORE_SET;
    if (!d3dmetal && set)
        return MS_NGX_CORE_CLEAR;
    return MS_NGX_CORE_KEEP;
}

/* wine reg <args>, in prefix, within 60 seconds. */
static bool run_reg(const char* wine, const char* prefix, char* const reg_args[]) {
    char* argv[16];
    size_t n = 0;
    pid_t pid;
    int status = 0;
    time_t deadline;
    argv[n++] = (char*)wine;
    argv[n++] = "reg";
    for (size_t i = 0; reg_args[i] && n < sizeof(argv) / sizeof(argv[0]) - 1; i++)
        argv[n++] = reg_args[i];
    argv[n] = NULL;
    pid = fork();
    if (pid < 0)
        return false;
    if (pid == 0) {
        setenv("WINEPREFIX", prefix, 1);
        setenv("WINEDEBUG", "-all", 1);
        unsetenv("WINEDLLOVERRIDES");
        (void)freopen("/dev/null", "w", stdout);
        (void)freopen("/dev/null", "w", stderr);
        execv(wine, argv);
        _exit(127);
    }
    deadline = time(NULL) + 60;
    for (;;) {
        pid_t done = waitpid(pid, &status, WNOHANG);
        if (done == pid)
            break;
        if (done < 0 && errno != EINTR)
            return false;
        if (time(NULL) >= deadline) {
            kill(pid, SIGKILL);
            (void)waitpid(pid, &status, 0);
            return false;
        }
        usleep(50000);
    }
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

bool ms_ngx_core_route(const char* home, const char* canonical_pipeline) {
    const char* env_prefix = getenv("WINEPREFIX");
    char prefix[PATH_MAX], wine[PATH_MAX], marker[PATH_MAX];
    bool d3dmetal = canonical_pipeline && !strcmp(canonical_pipeline, "d3dmetal");
    ms_ngx_core_action action;
    bool ok = true;
    struct stat st;
    if (!home)
        return false;
    if (env_prefix && env_prefix[0])
        snprintf(prefix, sizeof(prefix), "%s", env_prefix);
    else if ((size_t)snprintf(prefix, sizeof(prefix), "%s/prefix-steam", home) >= sizeof(prefix))
        return false;
    if (stat(prefix, &st) != 0 || !S_ISDIR(st.st_mode))
        return !d3dmetal; /* a prefix not made yet: its first D3DMetal launch sets it */
    action = ms_ngx_core_action_for(prefix, d3dmetal);
    if (action == MS_NGX_CORE_KEEP)
        return true;
    if ((size_t)snprintf(wine, sizeof(wine), "%s/runtime/wine/bin/wine", home) >= sizeof(wine) ||
        access(wine, X_OK) != 0 || !marker_path(prefix, marker, sizeof(marker)))
        return false;
    for (size_t i = 0; i < sizeof(NGX_KEYS) / sizeof(NGX_KEYS[0]); i++) {
        if (action == MS_NGX_CORE_SET) {
            char* const args[] = {"add", (char*)NGX_KEYS[i][0], "/v", (char*)NGX_KEYS[i][1], "/t", "REG_SZ", "/d",
                                  MS_NGX_CORE_DIR, "/f", NULL};
            ok = run_reg(wine, prefix, args) && ok;
        } else {
            char* const args[] = {"delete", (char*)NGX_KEYS[i][0], "/v", (char*)NGX_KEYS[i][1], "/f", NULL};
            (void)run_reg(wine, prefix, args); /* a value already gone is fine */
        }
    }
    if (action == MS_NGX_CORE_SET) {
        FILE* file;
        if (!ok || !(file = fopen(marker, "w")))
            return false;
        fputs("d3dmetal\n", file);
        return fclose(file) == 0;
    }
    return unlink(marker) == 0 || errno == ENOENT;
}
