#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE
#endif
#include "metalsharp_backend/wine_runtime.h"
#include "metalsharp_backend/json.h"
#include "metalsharp_backend/json_writer.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

#ifndef MS_BACKEND_VARIANT
#define MS_BACKEND_VARIANT "regular"
#endif

#define FACADE_MARKER  ".metalsharp-fex-facade"
/* The regular build's x86_64 runtime as one signed app (tools/package/build-wine-x86_64-app.sh) */
#define WINE_X86_64_APP "MetalSharp-Wine-x86_64.app"
#define FACADE_VERSION "3"

const char* ms_wine_variant(void) {
    return ms_wine_is_fex() ? "fex" : "regular";
}

const char* ms_wine_app_variant(void) {
    return !strcmp(MS_BACKEND_VARIANT, "fex") ? "fex" : "regular";
}

/* MetalSharp's home: $METALSHARP_HOME, else ~/.metalsharp */
static bool metalsharp_home(char* out, size_t out_size) {
    const char* configured = getenv("METALSHARP_HOME");
    const char* home = getenv("HOME");
    if (configured && configured[0])
        return snprintf(out, out_size, "%s", configured) < (int)out_size;
    return home && home[0] && snprintf(out, out_size, "%s/.metalsharp", home) < (int)out_size;
}

/* The active Wine runtime: the build's own (MetalSharp-Wine.app on the FEX build, MetalSharp-Wine-x86_64.app on the
 * regular one) until the user switches to the other one (config/active-runtime, ms_wine_switch_runtime_json), which
 * the backend reads once when it starts ($METALSHARP_ACTIVE_RUNTIME first). */
static int active_fex = -1;
static bool active_runtime_setting;

void ms_wine_use_active_runtime_setting(void) {
    active_runtime_setting = true;
    active_fex = -1;
}

static bool active_runtime_file(const char* home, char* out, size_t out_size) {
    return home && snprintf(out, out_size, "%s/config/active-runtime", home) < (int)out_size;
}

static int read_active_runtime(const char* home) {
    const char* forced = getenv("METALSHARP_ACTIVE_RUNTIME");
    char path[PATH_MAX], value[32] = "", home_buffer[PATH_MAX];
    FILE* file;
    if (forced && forced[0])
        snprintf(value, sizeof(value), "%s", forced);
    else if (active_runtime_setting &&
             (home || (metalsharp_home(home_buffer, sizeof(home_buffer)) && (home = home_buffer))) &&
             active_runtime_file(home, path, sizeof(path)) && (file = fopen(path, "r"))) {
        if (!fgets(value, sizeof(value), file))
            value[0] = '\0';
        fclose(file);
        value[strcspn(value, "\r\n \t")] = '\0';
    }
    if (!strcmp(value, "fex"))
        return 1;
    if (!strcmp(value, "regular"))
        return 0;
    return !strcmp(MS_BACKEND_VARIANT, "fex");
}

/* The x86_64 runtime laid out from MetalSharp-Wine-x86_64.app (setup's marker), not from the bundles */
static bool x86_64_app_runtime(void) {
    char home[PATH_MAX], marker[PATH_MAX];
    /* the backend's home, or one named explicitly (not a developer's own ~/.metalsharp under the tests) */
    if (!active_runtime_setting && !(getenv("METALSHARP_HOME") && getenv("METALSHARP_HOME")[0]))
        return false;
    return metalsharp_home(home, sizeof(home)) &&
           snprintf(marker, sizeof(marker), "%s/runtime/wine/.metalsharp-x86_64-app", home) < (int)sizeof(marker) &&
           access(marker, F_OK) == 0;
}

bool ms_wine_routes_lane(const char* pipeline) {
    if (ms_wine_is_fex())
        return true;
    /* MetalSharp-Wine-x86_64.app's Wine finds these lanes by itself from GRAPHICS_BACKEND (its ntdll's defaults and
     * VKD3D lane); an x86_64 runtime still from the bundles is routed as before */
    if (!x86_64_app_runtime())
        return false;
    return pipeline && (!strcmp(pipeline, "dxmt") || !strcmp(pipeline, "dxmt_32") || !strcmp(pipeline, "vkd3d") ||
                        !strcmp(pipeline, "dxvk") ||
                        !strcmp(pipeline, "d3dmetal") || !strcmp(pipeline, "wine_bare") || !strcmp(pipeline, "m32"));
}

bool ms_wine_metalsharp_launch_model(void) {
    return ms_wine_is_fex() || x86_64_app_runtime();
}

bool ms_wine_is_fex(void) {
    if (active_fex < 0)
        active_fex = read_active_runtime(NULL);
    return active_fex == 1;
}

void ms_wine_reload_active_runtime(void) {
    active_fex = -1;
}

const char* ms_wine_rules_file(void) {
    return ms_wine_is_fex() ? "mtsp-rules-fex.toml" : "mtsp-rules.toml";
}

static bool format_path(char* out, size_t out_size, const char* format, const char* a, const char* b) {
    int written = snprintf(out, out_size, format, a, b);
    return written >= 0 && (size_t)written < out_size;
}

bool ms_wine_app_path(const char* home, char* out, size_t out_size) {
    return home && out && format_path(out, out_size, "%s/runtime/%s", home, "MetalSharp-Wine.app");
}

bool ms_wine_app_contents(const char* home, char* out, size_t out_size) {
    return home && out && format_path(out, out_size, "%s/runtime/%s", home, "MetalSharp-Wine.app/Contents");
}

const char* ms_wine_route_pipeline(const char* pipeline) {
    if (!pipeline)
        return pipeline;
    /* The x86_64 runtime's lanes are D3DMetal, VKD3D-Proton (D3D12 to D3D8, with DXVK), Mullion (64- and 32-bit) and
     * OpenGL, as MetalSharp Wine's are: Game Porting Toolkit's own Wine gives way to D3DMetal, the D3D9 route to the
     * VKD3D lane's DXVK. */
    if (!ms_wine_is_fex()) {
        if (!x86_64_app_runtime())
            return pipeline;
        if (!strcmp(pipeline, "m13"))
            return "d3dmetal";
        /* One VKD3D / DXVK lane: VKD3D-Proton's D3D12 with the full DXVK (D3D11 to D3D8, 64- and 32-bit) beside it, so a
         * game saved on DXVK or the D3D9 route runs there */
        if (!strcmp(pipeline, "d3d9") || !strcmp(pipeline, "m9") || !strcmp(pipeline, "dxvk") ||
            !strcmp(pipeline, "dxvk_32"))
            return "vkd3d";
        if (!strcmp(pipeline, "dxmt_32"))
            return "dxmt";
        if (!strcmp(pipeline, "m32"))
            return "wine_bare";
        return pipeline;
    }
    /* D3DMetal is Apple's and x86_64-only: its D3D12 games take VKD3D-Proton. DXVK's D3D9 replaces
     * the D3D9 route (32-bit programs keep Wine's own D3D9, as DXVK is x86_64 here). */
    if (!strcmp(pipeline, "d3dmetal") || !strcmp(pipeline, "m13"))
        return "vkd3d";
    if (!strcmp(pipeline, "d3d9"))
        return "dxvk";
    /* FNA games run as Windows programs under FEX; the native Mono lane is the regular build's. */
    if (!strcmp(pipeline, "fna_arm64"))
        return "dxmt";
    return pipeline;
}

const char* ms_wine_fex_graphics_backend(const char* pipeline) {
    if (pipeline && (!strcmp(pipeline, "m32") || !strcmp(pipeline, "wine_bare")))
        return "wine";
    /* the x86_64 runtime's D3DMetal lane (MetalSharp Wine runs D3DMetal's games on VKD3D-Proton) */
    if (pipeline && !ms_wine_is_fex() && !strcmp(pipeline, "d3dmetal"))
        return "d3dmetal";
    if (pipeline && !strcmp(pipeline, "dxvk"))
        return "dxvk";
    if (pipeline && !strcmp(pipeline, "vkd3d"))
        return "vkd3d";
    return "dxmt";
}

static bool ends_with(const char* text, const char* suffix) {
    size_t text_length = strlen(text), suffix_length = strlen(suffix);
    return text_length >= suffix_length && !strcmp(text + text_length - suffix_length, suffix);
}

bool ms_wine_is_runtime_wine_path(const char* home, const char* path) {
    char root[PATH_MAX];
    size_t length;
    if (!home || !path)
        return false;
    if (format_path(root, sizeof(root), "%s/runtime/%s", home, "wine/") && !strncmp(path, root, strlen(root)))
        return ends_with(path, "/bin/wine") || ends_with(path, "/bin/wine64") ||
               ends_with(path, "/lib/wine/x86_64-unix/wine");
    if (!ms_wine_app_contents(home, root, sizeof(root)))
        return false;
    length = strlen(root);
    return !strncmp(path, root, length) &&
           (!strcmp(path + length, "/MacOS/wine") || !strcmp(path + length, "/lib/wine/aarch64-unix/wine"));
}

bool ms_resolved_path_within(const char* path, const char* root) {
    char resolved[PATH_MAX];
    size_t length;
    if (!path || !root || !root[0])
        return false;
    if (realpath(root, resolved))
        root = resolved;
    length = strlen(root);
    while (length > 1 && root[length - 1] == '/')
        length--;
    return !strncmp(path, root, length) && (path[length] == '\0' || path[length] == '/');
}

char* ms_data_path_for_keeping(const char* home, const char* path) {
    char resolved_home[PATH_MAX];
    size_t length;
    char* kept;
    if (!path)
        return NULL;
    if (!home || !realpath(home, resolved_home) || !strcmp(resolved_home, home))
        return strdup(path);
    length = strlen(resolved_home);
    if (strncmp(path, resolved_home, length) || (path[length] != '\0' && path[length] != '/'))
        return strdup(path);
    if (!(kept = malloc(strlen(home) + strlen(path + length) + 1)))
        return NULL;
    sprintf(kept, "%s%s", home, path + length);
    return kept;
}

bool ms_wine_process_root(const char* home, char* out, size_t out_size) {
    if (!home || !out)
        return false;
    if (ms_wine_is_fex())
        return ms_wine_app_path(home, out, out_size);
    return format_path(out, out_size, "%s/runtime/%s", home, "wine");
}

char* ms_wine_process_root_dup(const char* home) {
    char root[PATH_MAX];
    return ms_wine_process_root(home, root, sizeof(root)) ? strdup(root) : NULL;
}

bool ms_wine_server_path(const char* home, char* out, size_t out_size) {
    if (!home || !out)
        return false;
    if (ms_wine_is_fex())
        return format_path(out, out_size, "%s/runtime/%s", home, "MetalSharp-Wine.app/Contents/MacOS/wineserver");
    return format_path(out, out_size, "%s/runtime/%s", home, "wine/bin/wineserver");
}

static void set_error(char* error, size_t error_size, const char* message, const char* path) {
    if (error && error_size)
        snprintf(error, error_size, "%s%s%s", message, path ? ": " : "", path ? path : "");
}

/* Writes a file whole (temporary file and rename), so a wrapper is never seen half written. */
static bool write_file(const char* path, const char* text, mode_t mode) {
    char temporary[PATH_MAX];
    FILE* file;
    bool ok;
    if (!format_path(temporary, sizeof(temporary), "%s%s", path, ".tmp"))
        return false;
    file = fopen(temporary, "wb");
    if (!file)
        return false;
    ok = fputs(text, file) >= 0;
    ok = fclose(file) == 0 && ok;
    ok = ok && chmod(temporary, mode) == 0 && rename(temporary, path) == 0;
    if (!ok)
        unlink(temporary);
    return ok;
}

static bool ensure_dir(const char* path) {
    return mkdir(path, 0755) == 0 || errno == EEXIST;
}

/* The x86_64 runtime's library and DLL paths, Rosetta's switches and the D3DMetal lane's
 * variables mean nothing to MetalSharp Wine, and its loader finds its own modules; a stale
 * WINEDLLPATH would load x86_64 builtins into ARM64EC processes. */
static const char facade_prologue[] =
    "#!/bin/bash\n"
    "# METALSHARP_FEX_FACADE_V" FACADE_VERSION "\n"
    "# MetalSharp Wine (native arm64 Wine with FEX) runs from its signed app.\n"
    "MS_HOME=\"%s\"\n"
    "MS_APP=\"%s\"\n"
    "MS_WINE=\"$MS_APP/MacOS/wine\"\n"
    "unset DYLD_LIBRARY_PATH DYLD_FALLBACK_LIBRARY_PATH DYLD_INSERT_LIBRARIES WINEDLLPATH WINELOADER WINEDATADIR \\\n"
    "  CX_ROOT VK_ICD_FILENAMES VK_DRIVER_FILES DXMT_WINEMETAL_UNIXLIB DXMT_RUNTIME_DIR D3DMETAL_FRAMEWORK_PATH \\\n"
    "  D3DMETAL_RUNTIME_DIR WFDXCOMPAT_RUNTIME_DIR ROSETTA_ADVERTISE_AVX ROSETTA_X87_PATH\n"
    "export WINEPREFIX=\"${WINEPREFIX:-$MS_HOME/prefix-steam}\"\n"
    "export WINESERVER=\"$MS_APP/MacOS/wineserver\"\n"
    "export WINEDEBUG=\"${WINEDEBUG:--all}\"\n"
    "export DXVK_STATE_CACHE_PATH=\"${DXVK_STATE_CACHE_PATH:-$MS_HOME/dxvk-cache}\"\n"
    "export DXVK_LOG_PATH=\"${DXVK_LOG_PATH:-$MS_HOME/dxvk-logs}\"\n"
    "# GRAPHICS_BACKEND is dxmt (Mullion), dxvk, vkd3d or wine here; d3dmetal would mean wined3d.\n"
    "case \"${GRAPHICS_BACKEND:-}\" in\n"
    "  dxmt|dxvk|vkd3d|wine) ;;\n"
    "  '') unset GRAPHICS_BACKEND ;;\n"
    "  *) export GRAPHICS_BACKEND=dxmt ;;\n"
    "esac\n";

static const char facade_metalsharp_wine[] =
    "case \"${1:-}\" in\n"
    "  steam)\n"
    "    shift\n"
    "    export GRAPHICS_BACKEND=dxmt\n"
    "    # Steam starts as on Windows: its UI does not load with CEF flags or DLL overrides here\n"
    "    unset WINEDLLOVERRIDES\n"
    "    exec \"$MS_WINE\" \"$WINEPREFIX/drive_c/Program Files (x86)/Steam/steam.exe\" \"$@\"\n"
    "    ;;\n"
    "  gptk)\n"
    "    echo \"ERROR: GPTK runs on the x86_64 build of MetalSharp only\" >&2\n"
    "    exit 1\n"
    "    ;;\n"
    "  game|dxmt)\n"
    "    shift\n"
    "    export GRAPHICS_BACKEND=dxmt\n"
    "    exec \"$MS_WINE\" \"$@\"\n"
    "    ;;\n"
    "  dxvk)\n"
    "    shift\n"
    "    export GRAPHICS_BACKEND=dxvk\n"
    "    exec \"$MS_WINE\" \"$@\"\n"
    "    ;;\n"
    "  *)\n"
    "    exec \"$MS_WINE\" \"$@\"\n"
    "    ;;\n"
    "esac\n";

/* bin/<name> runs `wine <program>`, as the x86_64 runtime's Wine program launchers do. */
static const char* const facade_programs[] = {"wineboot", "msiexec",  "regedit",  "regsvr32", "winecfg",
                                              "wineconsole", "winedbg", "winefile", "winepath", "notepad"};
/* bin/<name> runs the app's own Contents/MacOS/<name>. */
static const char* const facade_tools[] = {"wineserver", "innoextract", "cabextract"};

static bool write_facade_script(const char* bin, const char* name, const char* prologue, const char* body) {
    char path[PATH_MAX];
    size_t size = strlen(prologue) + strlen(body) + 1;
    char* text = malloc(size);
    bool ok;
    if (!text || !format_path(path, sizeof(path), "%s/%s", bin, name)) {
        free(text);
        return false;
    }
    snprintf(text, size, "%s%s", prologue, body);
    ok = write_file(path, text, 0755);
    free(text);
    return ok;
}

bool ms_wine_fex_write_facade(const char* home, char* error, size_t error_size) {
    char contents[PATH_MAX], root[PATH_MAX], kept[PATH_MAX], bin[PATH_MAX], etc[PATH_MAX], target[PATH_MAX],
        marker[PATH_MAX];
    char* prologue;
    size_t prologue_size;
    struct stat info;
    bool ok = true;
    if (!ms_wine_app_contents(home, contents, sizeof(contents)) ||
        !format_path(root, sizeof(root), "%s/runtime/%s", home, "wine") ||
        !format_path(kept, sizeof(kept), "%s/runtime/%s", home, "wine-x86_64") ||
        !format_path(bin, sizeof(bin), "%s/%s", root, "bin") || !format_path(etc, sizeof(etc), "%s/%s", root, "etc") ||
        !format_path(marker, sizeof(marker), "%s/%s", root, FACADE_MARKER)) {
        set_error(error, error_size, "path too long", home);
        return false;
    }
    if (access(contents, R_OK) != 0) {
        set_error(error, error_size, "MetalSharp Wine is not installed", contents);
        return false;
    }
    /* A 0.80.0 install's x86_64 runtime stays as runtime/wine-x86_64, for the x86_64 build to
     * take back, instead of being mixed with the facade. */
    if (lstat(root, &info) == 0 && access(marker, F_OK) != 0) {
        if (lstat(kept, &info) == 0 || rename(root, kept) != 0) {
            set_error(error, error_size, "could not move the x86_64 runtime aside", root);
            return false;
        }
    }
    if (!ensure_dir(root) || !ensure_dir(bin) || !ensure_dir(etc)) {
        set_error(error, error_size, "could not create", root);
        return false;
    }
    for (int i = 0; i < 2; i++) {
        const char* name = i ? "share" : "lib";
        char link[PATH_MAX];
        if (!format_path(link, sizeof(link), "%s/%s", root, name) ||
            !format_path(target, sizeof(target), "%s/%s", contents, name)) {
            set_error(error, error_size, "path too long", root);
            return false;
        }
        if (lstat(link, &info) == 0 && (!S_ISLNK(info.st_mode) || unlink(link) != 0)) {
            set_error(error, error_size, "not a link to MetalSharp Wine", link);
            return false;
        }
        if (symlink(target, link) != 0) {
            set_error(error, error_size, "could not link", link);
            return false;
        }
    }
    prologue_size = sizeof(facade_prologue) + strlen(home) + strlen(contents);
    prologue = malloc(prologue_size);
    if (!prologue) {
        set_error(error, error_size, "out of memory", NULL);
        return false;
    }
    snprintf(prologue, prologue_size, facade_prologue, home, contents);
    ok = write_facade_script(bin, "metalsharp-wine", prologue, facade_metalsharp_wine);
    ok = write_facade_script(bin, "wine", prologue, "exec \"$MS_WINE\" \"$@\"\n") && ok;
    ok = write_facade_script(bin, "wine64", prologue, "exec \"$MS_WINE\" \"$@\"\n") && ok;
    for (size_t i = 0; i < sizeof(facade_programs) / sizeof(facade_programs[0]); i++) {
        char body[128];
        snprintf(body, sizeof(body), "exec \"$MS_WINE\" %s \"$@\"\n", facade_programs[i]);
        ok = write_facade_script(bin, facade_programs[i], prologue, body) && ok;
    }
    for (size_t i = 0; i < sizeof(facade_tools) / sizeof(facade_tools[0]); i++) {
        char body[128];
        snprintf(body, sizeof(body), "exec \"$MS_APP/MacOS/%s\" \"$@\"\n", facade_tools[i]);
        ok = write_facade_script(bin, facade_tools[i], prologue, body) && ok;
    }
    free(prologue);
    ok = ok && write_file(marker, FACADE_VERSION "\n", 0644);
    if (!ok)
        set_error(error, error_size, "could not write the MetalSharp Wine wrappers", bin);
    return ok;
}

bool ms_wine_fex_facade_ready(const char* home) {
    char marker[PATH_MAX], wrapper[PATH_MAX], contents[PATH_MAX], wine[PATH_MAX];
    FILE* file;
    char version[16] = "";
    if (!format_path(marker, sizeof(marker), "%s/runtime/%s", home, "wine/" FACADE_MARKER) ||
        !format_path(wrapper, sizeof(wrapper), "%s/runtime/%s", home, "wine/bin/metalsharp-wine") ||
        !ms_wine_app_contents(home, contents, sizeof(contents)) ||
        !format_path(wine, sizeof(wine), "%s/%s", contents, "MacOS/wine"))
        return false;
    file = fopen(marker, "rb");
    if (!file)
        return false;
    if (!fgets(version, sizeof(version), file))
        version[0] = '\0';
    fclose(file);
    return !strcmp(version, FACADE_VERSION "\n") && access(wrapper, X_OK) == 0 && access(wine, X_OK) == 0;
}

/* Runs argv with WINEPREFIX set (when prefix is given) and waits up to `seconds`; a child that
 * outlives it is killed. Its output goes to `log` (appended) when one is given. */
static bool run_wait(const char* const* argv, const char* prefix, const char* log, unsigned seconds) {
    pid_t pid = fork();
    int status = 0;
    time_t deadline;
    if (pid < 0)
        return false;
    if (pid == 0) {
        int fd = log ? open(log, O_WRONLY | O_CREAT | O_APPEND, 0644) : open("/dev/null", O_WRONLY);
        if (fd >= 0) {
            dup2(fd, STDOUT_FILENO);
            dup2(fd, STDERR_FILENO);
            close(fd);
        }
        if (prefix)
            setenv("WINEPREFIX", prefix, 1);
        setenv("WINEDEBUG", "-all", 1);
        setenv("WINEDEBUGGER", "none", 1);
        setpgid(0, 0);
        execv(argv[0], (char* const*)argv);
        _exit(127);
    }
    deadline = time(NULL) + (time_t)seconds;
    for (;;) {
        pid_t done = waitpid(pid, &status, WNOHANG);
        if (done == pid)
            break;
        if (done < 0 && errno != EINTR)
            return false;
        if (time(NULL) >= deadline) {
            kill(-pid, SIGKILL);
            kill(pid, SIGKILL);
            while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
            }
            return false;
        }
        usleep(100000);
    }
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

char* ms_app_resources_path(const char* name) {
    const char* override = getenv("METALSHARP_APP_RESOURCES");
    char path[PATH_MAX], resolved[PATH_MAX], candidate[PATH_MAX];
    if (override && override[0])
        return format_path(candidate, sizeof(candidate), "%s/%s", override, name) && access(candidate, R_OK) == 0
                   ? strdup(candidate)
                   : NULL;
#ifdef __APPLE__
    {
        uint32_t size = sizeof(path);
        /* The backend is MetalSharp.app/Contents/Resources/runtime/metalsharp-backend. */
        if (_NSGetExecutablePath(path, &size) == 0 && realpath(path, resolved)) {
            char* slash = strrchr(resolved, '/');
            if (slash) {
                *slash = '\0';
                if (format_path(candidate, sizeof(candidate), "%s/../%s", resolved, name) &&
                    realpath(candidate, path) && access(path, R_OK) == 0)
                    return strdup(path);
            }
        }
    }
#endif
    (void)resolved;
    if (format_path(candidate, sizeof(candidate), "%s/%s", "/Applications/MetalSharp.app/Contents/Resources", name) &&
        access(candidate, R_OK) == 0)
        return strdup(candidate);
    return NULL;
}

/* <name> in MetalSharp.app's Resources, else the other build's runtime, which MetalSharp.app does not carry,
 * downloaded (ms_wine_runtime_download); $<override> names another. */
static char* app_source(const char* override_name, const char* name) {
    const char* override = getenv(override_name);
    char candidate[PATH_MAX], home[PATH_MAX];
    char* found;
    if (override && override[0])
        return access(override, R_OK) == 0 ? strdup(override) : NULL;
    if ((found = ms_app_resources_path(name)))
        return found;
    if (metalsharp_home(home, sizeof(home)) &&
        format_path(candidate, sizeof(candidate), "%s/downloads/%s", home, name) && access(candidate, R_OK) == 0)
        return strdup(candidate);
    return NULL;
}

char* ms_wine_fex_app_source(void) {
    return app_source("METALSHARP_WINE_APP_SOURCE", "MetalSharp-Wine.app");
}

char* ms_wine_x86_64_app_source(void) {
    return app_source("METALSHARP_WINE_X86_64_APP_SOURCE", WINE_X86_64_APP);
}

bool ms_wine_x86_64_app_contents(const char* home, char* out, size_t out_size) {
    return home && out && format_path(out, out_size, "%s/runtime/%s", home, WINE_X86_64_APP "/Contents");
}

/* The code directory hash (CDHash) identifies a MetalSharp Wine build. */
static bool app_identity(const char* app, char* out, size_t out_size) {
    char command[PATH_MAX * 2];
    FILE* pipe;
    size_t used = 0;
    snprintf(command, sizeof(command), "/usr/bin/codesign -dvvv '%s' 2>&1 | /usr/bin/grep '^CDHash='", app);
    pipe = popen(command, "r");
    if (!pipe)
        return false;
    used = fread(out, 1, out_size - 1, pipe);
    out[used] = '\0';
    return pclose(pipe) == 0 && used > 0;
}

static bool app_signature_valid(const char* app, const char* log) {
    const char* const verify[] = {"/usr/bin/codesign", "--verify", "--deep", "--strict", app, NULL};
    return run_wait(verify, NULL, log, 300);
}

static bool set_tree_writable(const char* path, bool writable) {
    const char* const command[] = {"/bin/chmod", "-R", writable ? "u+w" : "a-w", path, NULL};
    return run_wait(command, NULL, NULL, 120);
}

static bool remove_tree(const char* path) {
    const char* const command[] = {"/bin/rm", "-rf", path, NULL};
    struct stat info;
    if (lstat(path, &info) != 0)
        return true;
    (void)set_tree_writable(path, true);
    return run_wait(command, NULL, NULL, 300) && lstat(path, &info) != 0;
}

int ms_wine_fex_lock(const char* home, bool wait) {
    char runtime[PATH_MAX], path[PATH_MAX];
    int fd;
    if (!home || !format_path(runtime, sizeof(runtime), "%s/%s", home, "runtime") ||
        !format_path(path, sizeof(path), "%s/%s", runtime, ".metalsharp-wine.lock") || !ensure_dir(runtime))
        return -1;
    fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0)
        return -1;
    while (flock(fd, wait ? LOCK_EX : LOCK_EX | LOCK_NB) != 0) {
        if (errno != EINTR) {
            close(fd);
            return -1;
        }
    }
    return fd;
}

void ms_wine_fex_unlock(int fd) {
    if (fd >= 0)
        close(fd);
}

bool ms_wine_fex_refresh_blocked(const char* home) {
    char path[PATH_MAX], text[32] = "";
    FILE* file;
    long owner;
    if (!home)
        return true;
    if (format_path(path, sizeof(path), "%s/%s", home, ".post-update-migration") && access(path, F_OK) == 0)
        return true;
    /* migration.lock holds the migrating backend's pid; one left by a backend that is gone is stale. */
    if (!format_path(path, sizeof(path), "%s/%s", home, "migration.lock") || !(file = fopen(path, "r")))
        return false;
    if (!fgets(text, sizeof(text), file))
        text[0] = '\0';
    fclose(file);
    owner = strtol(text, NULL, 10);
    return owner > 0 && (kill((pid_t)owner, 0) == 0 || errno == EPERM);
}

static bool install_app_locked(const char* home, char* source, const char* name, char* error, size_t error_size);

bool ms_wine_fex_install_app(const char* home, char* error, size_t error_size) {
    int lock = ms_wine_fex_lock(home, true);
    bool ok = install_app_locked(home, ms_wine_fex_app_source(), "MetalSharp-Wine.app", error, error_size) &&
              ms_wine_fex_write_facade(home, error, error_size);
    ms_wine_fex_unlock(lock);
    return ok;
}

/* MetalSharp-Wine-x86_64.app into runtime/, as MetalSharp-Wine.app on the fex build: copied while its code signature
 * differs from the installed one, verified, and read-only. Setup lays out the x86_64 runtime from it. */
bool ms_wine_x86_64_install_app(const char* home, char* error, size_t error_size) {
    int lock = ms_wine_fex_lock(home, true);
    bool ok = install_app_locked(home, ms_wine_x86_64_app_source(), WINE_X86_64_APP, error, error_size);
    ms_wine_fex_unlock(lock);
    return ok;
}

/* The other runtime's Wine app, when that runtime is installed too (kept aside while this one is active), updated from
 * the one MetalSharp.app carries, so an update brings both runtimes up to date and not only the active one. Only the
 * app is replaced: what its runtime laid out stays aside, and is laid out again from the new app when the user switches
 * to it (its identity differs). A runtime that is not installed stays uninstalled. */
bool ms_wine_refresh_inactive_app(const char* home, char* error, size_t error_size) {
    const char* other = ms_wine_is_fex() ? "regular" : "fex";
    bool fex = !strcmp(other, "fex");
    char* source;
    int lock;
    bool ok;
    if (!home || !ms_wine_runtime_installed(home, other))
        return true;
    if (!(source = fex ? ms_wine_fex_app_source() : ms_wine_x86_64_app_source()))
        return true; /* this MetalSharp carries no newer one */
    if ((lock = ms_wine_fex_lock(home, true)) < 0) {
        free(source);
        set_error(error, error_size, "could not lock MetalSharp Wine", home);
        return false;
    }
    ok = install_app_locked(home, source, fex ? "MetalSharp-Wine.app" : WINE_X86_64_APP, error, error_size);
    ms_wine_fex_unlock(lock);
    return ok;
}

/* Takes source (freed here) */
static bool install_app_locked(const char* home, char* source, const char* name, char* error, size_t error_size) {
    char app[PATH_MAX], staging[PATH_MAX], old[PATH_MAX], runtime[PATH_MAX], log[PATH_MAX], logs[PATH_MAX];
    char source_identity[256] = "", installed_identity[256] = "", message[128];
    bool ok = false;
    if (!source) {
        snprintf(message, sizeof(message), "this MetalSharp does not carry %s; reinstall MetalSharp", name);
        set_error(error, error_size, message, NULL);
        return false;
    }
    if (!format_path(app, sizeof(app), "%s/runtime/%s", home, name) ||
        !format_path(staging, sizeof(staging), "%s%s", app, ".new") ||
        !format_path(old, sizeof(old), "%s%s", app, ".old") ||
        !format_path(runtime, sizeof(runtime), "%s/%s", home, "runtime") ||
        !format_path(logs, sizeof(logs), "%s/%s", home, "logs") ||
        !format_path(log, sizeof(log), "%s/%s", logs, "setup-metalsharp-wine.log")) {
        set_error(error, error_size, "path too long", home);
        goto done;
    }
    (void)ensure_dir(home);
    (void)ensure_dir(runtime);
    (void)ensure_dir(logs);
    if (!app_identity(source, source_identity, sizeof(source_identity))) {
        snprintf(message, sizeof(message), "%s is not signed", name);
        set_error(error, error_size, message, source);
        goto done;
    }
    if (!(access(app, R_OK) == 0 && app_identity(app, installed_identity, sizeof(installed_identity)) &&
          !strcmp(source_identity, installed_identity) && app_signature_valid(app, log))) {
        const char* const copy[] = {"/usr/bin/ditto", source, staging, NULL};
        /* ditto gives a directory its mode before filling it, so it cannot copy a read-only app (one this backend
         * installed, copied on); cp -Rp sets the modes after */
        const char* const copy_read_only[] = {"/bin/cp", "-Rp", source, staging, NULL};
        if (!remove_tree(staging) ||
            (!run_wait(copy, NULL, log, 900) && (!remove_tree(staging) || !run_wait(copy_read_only, NULL, log, 900)))) {
            set_error(error, error_size, "could not copy MetalSharp Wine", staging);
            goto done;
        }
        if (!app_signature_valid(staging, log)) {
            (void)remove_tree(staging);
            set_error(error, error_size, "MetalSharp Wine's signature did not verify; see logs/setup-metalsharp-wine.log",
                      NULL);
            goto done;
        }
        if (!remove_tree(old) || (access(app, F_OK) == 0 && rename(app, old) != 0) || rename(staging, app) != 0) {
            set_error(error, error_size, "could not install MetalSharp Wine", app);
            goto done;
        }
        (void)remove_tree(old);
    }
    /* Read-only, so nothing MetalSharp does later can write into the sealed bundle. */
    (void)set_tree_writable(app, false);
    ok = true;
done:
    free(source);
    return ok;
}

static bool prefix_file(const char* prefix, const char* name, char* out, size_t out_size) {
    return format_path(out, out_size, "%s/%s", prefix, name);
}

bool ms_wine_fex_prefix_ready(const char* prefix) {
    char path[PATH_MAX];
    return prefix && prefix_file(prefix, ".metalsharp-prefix-ready", path, sizeof(path)) && access(path, F_OK) == 0;
}

/* Wine has builtin msvcp140 and vcruntime140, so the DLLs prove nothing; the installers register
 * each runtime under VisualStudio\14.0\VC\Runtimes. */
static bool vcpp_registered(const char* prefix) {
    static const char x64_key[] = "[Software\\\\Microsoft\\\\VisualStudio\\\\14.0\\\\VC\\\\Runtimes\\\\x64]";
    static const char x86_key[] =
        "[Software\\\\Wow6432Node\\\\Microsoft\\\\VisualStudio\\\\14.0\\\\VC\\\\Runtimes\\\\x86]";
    char path[PATH_MAX];
    FILE* file;
    char line[1024];
    bool x64 = false, x86 = false;
    if (!prefix_file(prefix, "system.reg", path, sizeof(path)) || !(file = fopen(path, "rb")))
        return false;
    while (!(x64 && x86) && fgets(line, sizeof(line), file)) {
        x64 = x64 || !strncmp(line, x64_key, sizeof(x64_key) - 1);
        x86 = x86 || !strncmp(line, x86_key, sizeof(x86_key) - 1);
    }
    fclose(file);
    return x64 && x86;
}

/* VC++ 2015-2022 registered for an architecture ("x64" or "arm64") under VC\Runtimes\<arch> */
static bool vcrun_registered(const char* prefix, const char* arch) {
    char key[160], path[PATH_MAX], line[1024];
    FILE* file;
    bool found = false;
    snprintf(key, sizeof(key), "[software\\\\microsoft\\\\visualstudio\\\\14.0\\\\vc\\\\runtimes\\\\%s]", arch);
    if (!prefix_file(prefix, "system.reg", path, sizeof(path)) || !(file = fopen(path, "rb")))
        return false;
    while (!found && fgets(line, sizeof(line), file)) {
        size_t i = 0;
        for (; key[i] && line[i] && tolower((unsigned char)line[i]) == key[i]; i++)
            ;
        found = !key[i];
    }
    fclose(file);
    return found;
}

/* Whether System32's msvcp140.dll is Microsoft's rather than a copy of Wine's builtin ("Wine builtin DLL" in its DOS
 * stub). The registration alone proves nothing: going through the x86_64 runtime and back, or a wineboot, can leave
 * Wine's copies under Microsoft's registration, and Unreal's prerequisite check reads the files. */
static bool vcpp_files_microsoft(const char* prefix) {
    char path[PATH_MAX];
    unsigned char head[0x60];
    size_t length = 0;
    FILE* file;
    if (!prefix_file(prefix, "drive_c/windows/system32/msvcp140.dll", path, sizeof(path)) || !(file = fopen(path, "rb")))
        return false;
    length = fread(head, 1, sizeof(head), file);
    fclose(file);
    return length >= 0x50 && head[0] == 'M' && head[1] == 'Z' && memcmp(head + 0x40, "Wine builtin DLL", 16) != 0;
}

static void (*prefix_progress)(void* context, const char* part, const char* message);
static void* prefix_progress_context;

void ms_wine_set_prefix_progress(void (*progress)(void* context, const char* part, const char* message),
                                 void* context) {
    prefix_progress = progress;
    prefix_progress_context = context;
}

static void report_prefix_progress(const char* part, const char* message) {
    if (prefix_progress)
        prefix_progress(prefix_progress_context, part, message);
}

/* The prefix holds the VC++ runtime the active Wine runtime needs: the ARM64 one (ARM64X DLLs) under FEX, the x64
 * one under the x86_64 runtime, registered and with Microsoft's files in System32. */
static bool vcrun_ready(const char* prefix, bool fex) {
    return vcrun_registered(prefix, fex ? "arm64" : "x64") && vcpp_files_microsoft(prefix);
}

static const char* const vcrun_dlls[] = {"concrt140.dll",      "msvcp140.dll",       "msvcp140_1.dll",
                                         "msvcp140_2.dll",     "msvcp140_atomic_wait.dll",
                                         "msvcp140_codecvt_ids.dll", "vcomp140.dll", "vcruntime140.dll",
                                         "vcruntime140_1.dll"};

static bool is_prefix(const char* prefix);

static bool copy_file_to(const char* source, const char* target);

/* Whether path is a PE file and not one of Wine's builtins ("Wine builtin DLL" in its DOS stub) */
static bool microsoft_pe(const char* path) {
    unsigned char head[0x60];
    size_t length;
    FILE* file = fopen(path, "rb");
    if (!file)
        return false;
    length = fread(head, 1, sizeof(head), file);
    fclose(file);
    return length >= 0x50 && head[0] == 'M' && head[1] == 'Z' && memcmp(head + 0x40, "Wine builtin DLL", 16) != 0;
}

static bool starts_with_cab(const char* path) {
    char magic[4] = "";
    FILE* file = fopen(path, "rb");
    bool cab;
    if (!file)
        return false;
    cab = fread(magic, 1, sizeof(magic), file) == sizeof(magic) && !memcmp(magic, "MSCF", 4);
    fclose(file);
    return cab;
}

/* Microsoft's ARM64 VC++ 2015-2022 DLLs unpacked from vc_redist.arm64.exe into <home>/cache/redist/vcrun-arm64,
 * once: the Burn bundle's container with cabextract, then each cabinet in it. Its bootstrapper is 32-bit x86, which
 * FEX's WoW64 cannot run, and its MSI refuses a prefix that has a newer version registered over Wine's files, so
 * MetalSharp places the DLLs itself. */
static bool vcrun_arm64_unpacked(const char* home, const char* log, char* out, size_t out_size) {
    char installer[PATH_MAX], partial[PATH_MAX], bundle[PATH_MAX], contents[PATH_MAX], cabextract[PATH_MAX],
        probe[PATH_MAX], cache[PATH_MAX];
    DIR* dir;
    struct dirent* entry;
    if (!format_path(cache, sizeof(cache), "%s/%s", home, "cache/redist") ||
        !format_path(installer, sizeof(installer), "%s/%s", cache, "vc_redist.arm64.exe") ||
        !format_path(partial, sizeof(partial), "%s/%s", cache, "vc_redist.arm64.part") ||
        !format_path(bundle, sizeof(bundle), "%s/%s", cache, "vcrun-arm64-bundle") ||
        !format_path(out, out_size, "%s/%s", cache, "vcrun-arm64") ||
        !format_path(probe, sizeof(probe), "%s/%s", out, "msvcp140.dll_arm64") ||
        !ms_wine_app_contents(home, contents, sizeof(contents)) ||
        !format_path(cabextract, sizeof(cabextract), "%s/%s", contents, "MacOS/cabextract"))
        return false;
    if (microsoft_pe(probe))
        return true;
    if (!ensure_dir(cache))
        return false;
    if (!microsoft_pe(installer)) {
        const char* const fetch[] = {"/usr/bin/curl", "-fsSL", "--retry", "3", "-o", partial,
                                     "https://aka.ms/vs/17/release/vc_redist.arm64.exe", NULL};
        report_prefix_progress("VC++ 2022", "Downloading the VC++ 2022 runtime (ARM64)...");
        (void)unlink(partial);
        if (!run_wait(fetch, NULL, log, 600) || !microsoft_pe(partial) || rename(partial, installer) != 0) {
            (void)unlink(partial);
            return false;
        }
    }
    (void)remove_tree(bundle);
    (void)remove_tree(out);
    {
        const char* const unpack[] = {cabextract, "-q", "-d", bundle, installer, NULL};
        if (!ensure_dir(bundle) || !ensure_dir(out) || !run_wait(unpack, NULL, log, 300) || !(dir = opendir(bundle)))
            return false;
    }
    while ((entry = readdir(dir)) != NULL) {
        char file[PATH_MAX];
        if (entry->d_name[0] == '.' || !format_path(file, sizeof(file), "%s/%s", bundle, entry->d_name) ||
            !starts_with_cab(file))
            continue;
        {
            const char* const unpack[] = {cabextract, "-q", "-d", out, file, NULL};
            (void)run_wait(unpack, NULL, log, 300);
        }
    }
    closedir(dir);
    (void)remove_tree(bundle);
    return microsoft_pe(probe);
}

/* The ARM64 VC++ runtime into the prefix: Microsoft's DLLs into System32 (each replacing Wine's copy), and its
 * VC\Runtimes\arm64 registration where there is none (Unreal's prerequisite check reads both). */
static bool place_vcrun_arm64(const char* home, const char* prefix, const char* log) {
    char unpacked[PATH_MAX], system32[PATH_MAX], wine[PATH_MAX];
    bool ok = true;
    if (!vcrun_arm64_unpacked(home, log, unpacked, sizeof(unpacked)) ||
        !format_path(system32, sizeof(system32), "%s/%s", prefix, "drive_c/windows/system32"))
        return false;
    report_prefix_progress("VC++ 2022", "Installing the VC++ 2022 runtime (ARM64)...");
    for (size_t i = 0; i < sizeof(vcrun_dlls) / sizeof(vcrun_dlls[0]); i++) {
        char source[PATH_MAX], target[PATH_MAX];
        if (!format_path(target, sizeof(target), "%s/%s", system32, vcrun_dlls[i]))
            return false;
        if (!(format_path(source, sizeof(source), "%s/%s_arm64", unpacked, vcrun_dlls[i]) && microsoft_pe(source)) &&
            !(format_path(source, sizeof(source), "%s/%s_system_arm64", unpacked, vcrun_dlls[i]) && microsoft_pe(source)))
            continue;
        if (!microsoft_pe(target) && !copy_file_to(source, target))
            ok = false;
    }
    if (ok && !vcrun_registered(prefix, "arm64")) {
        char reg[PATH_MAX];
        FILE* file;
        if (format_path(wine, sizeof(wine), "%s/runtime/%s", home, "wine/bin/wine") &&
            format_path(reg, sizeof(reg), "%s/%s", prefix, "drive_c/windows/temp/metalsharp-vcrun-arm64.reg") &&
            (file = fopen(reg, "w"))) {
            const char* const import[] = {wine, "reg", "import", "C:\\windows\\temp\\metalsharp-vcrun-arm64.reg", NULL};
            fputs("Windows Registry Editor Version 5.00\r\n\r\n"
                  "[HKEY_LOCAL_MACHINE\\Software\\Microsoft\\VisualStudio\\14.0\\VC\\Runtimes\\arm64]\r\n"
                  "\"Installed\"=dword:00000001\r\n\"Major\"=dword:0000000e\r\n\"Minor\"=dword:0000002c\r\n"
                  "\"Bld\"=dword:0000898b\r\n\"Rbld\"=dword:00000000\r\n\"Version\"=\"v14.44.35211.00\"\r\n",
                  file);
            if (fclose(file) == 0)
                (void)run_wait(import, prefix, log, 120);
            (void)unlink(reg);
        }
    }
    return ok && vcpp_files_microsoft(prefix);
}

/* The x64 VC++ runtime for the x86_64 runtime: Microsoft's x64 DLLs (MetalSharp-Wine-x86_64.app carries them) over
 * any ARM64 or Wine copy, the ARM64 ones kept aside for the next switch back (ms_wine_fit_prefix_vcrun). */
static bool place_vcrun_x64(const char* home, const char* prefix) {
    (void)prefix;
    (void)ms_wine_fit_prefix_vcrun(home, true);
    return vcpp_files_microsoft(prefix);
}

/* The shared VC++ repair: the prefix gets the VC++ runtime the active Wine runtime needs, Microsoft's files in
 * System32 (ARM64X under FEX, x64 under the x86_64 runtime); a prefix that has them is left alone. No installer runs:
 * MetalSharp places the files itself, so it does not depend on Microsoft's bootstrapper running under Wine. */
bool ms_wine_repair_vcrun(const char* home, const char* prefix, char* error, size_t error_size) {
    bool fex = ms_wine_is_fex();
    char log[PATH_MAX];
    if (!home || !prefix || !is_prefix(prefix)) {
        set_error(error, error_size, "no prefix", prefix);
        return false;
    }
    if (fex ? vcrun_ready(prefix, true) : vcpp_files_microsoft(prefix))
        return true;
    if (!format_path(log, sizeof(log), "%s/logs/%s", home, "prefix-setup.log")) {
        set_error(error, error_size, "path too long", home);
        return false;
    }
    if (!(fex ? place_vcrun_arm64(home, prefix, log) : place_vcrun_x64(home, prefix))) {
        set_error(error, error_size,
                  fex ? "could not install the VC++ 2022 runtime (ARM64); see logs/prefix-setup.log"
                      : "could not install the VC++ 2022 runtime (x64)",
                  prefix);
        return false;
    }
    return true;
}

bool ms_wine_fex_prepare_prefix(const char* home, const char* prefix, bool force, char* error, size_t error_size) {
    static const char* const corefonts[] = {"andale32.exe", "arial32.exe",  "arialb32.exe", "comic32.exe",
                                            "courie32.exe", "georgi32.exe", "impact32.exe", "times32.exe",
                                            "trebuc32.exe", "verdan32.exe", "webdin32.exe"};
    char wine[PATH_MAX], server[PATH_MAX], cabextract[PATH_MAX], share[PATH_MAX], fonts[PATH_MAX], log[PATH_MAX],
        path[PATH_MAX], contents[PATH_MAX];
    if (!home || !prefix) {
        set_error(error, error_size, "no prefix", NULL);
        return false;
    }
    if (!force && ms_wine_fex_prefix_ready(prefix) && vcrun_ready(prefix, true))
        return true;
    if (!format_path(wine, sizeof(wine), "%s/runtime/%s", home, "wine/bin/wine") ||
        !ms_wine_server_path(home, server, sizeof(server)) || !ms_wine_app_contents(home, contents, sizeof(contents)) ||
        !format_path(cabextract, sizeof(cabextract), "%s/%s", contents, "MacOS/cabextract") ||
        !format_path(share, sizeof(share), "%s/%s", contents, "share/metalsharp/redist") ||
        !format_path(fonts, sizeof(fonts), "%s/%s", prefix, "drive_c/windows/Fonts") ||
        !format_path(log, sizeof(log), "%s/logs/%s", home, "prefix-setup.log")) {
        set_error(error, error_size, "path too long", prefix);
        return false;
    }
    if (!ms_wine_fex_facade_ready(home)) {
        set_error(error, error_size, "MetalSharp Wine is not installed", NULL);
        return false;
    }
    /* A prefix set up before MetalSharp installed the ARM64 VC++ runtime (or one the x86_64 runtime set up) gets just
     * that; it does not hold up the launch when it cannot be installed (offline), and is tried again next time. */
    if (!force && ms_wine_fex_prefix_ready(prefix)) {
        char vcrun_error[PATH_MAX + 128];
        if (!ms_wine_repair_vcrun(home, prefix, vcrun_error, sizeof(vcrun_error)))
            fprintf(stderr, "warning: %s\n", vcrun_error);
        return true;
    }
    (void)ensure_dir(prefix);
    report_prefix_progress("Windows prefix", "Creating the Windows prefix...");
    {
        /* -r: without it wineboot also runs the prefix's Run keys, which start Steam's client and
         * keep the wineserver -w below waiting. */
        const char* const boot[] = {wine, "wineboot", "-u", "-r", NULL};
        const char* const wait_server[] = {server, "-w", NULL};
        if (!run_wait(boot, prefix, log, 600) || !run_wait(wait_server, prefix, log, 300)) {
            set_error(error, error_size, "wineboot could not prepare the prefix; see logs/prefix-setup.log", prefix);
            return false;
        }
    }
    /* VC++ 2022: its bootstrappers are 32-bit, so they run through FEX's WoW64. */
    for (int x86 = 0; x86 < 2; x86++) {
        char installer[PATH_MAX];
        const char* const wait_server[] = {server, "-w", NULL};
        if (!format_path(installer, sizeof(installer), "%s/%s", share,
                         x86 ? "vcrun2022/vc_redist.x86.exe" : "vcrun2022/vc_redist.x64.exe"))
            continue;
        if (access(installer, R_OK) == 0) {
            const char* const install[] = {wine, installer, "/install", "/quiet", "/norestart", NULL};
            report_prefix_progress(x86 ? "VC++ 2022 (x86)" : "VC++ 2022 (x64)",
                                   x86 ? "Installing the VC++ 2022 runtime (x86)..."
                                       : "Installing the VC++ 2022 runtime (x64)...");
            /* 3010 (reboot required) also counts as installed; the DLL check below decides. */
            (void)run_wait(install, prefix, log, 900);
            (void)run_wait(wait_server, prefix, log, 300);
        }
    }
    if (!vcpp_registered(prefix)) {
        set_error(error, error_size, "the VC++ 2022 runtime did not install; see logs/prefix-setup.log", prefix);
        return false;
    }
    /* and Microsoft's ARM64 one, downloaded (MetalSharp Wine does not carry it) */
    {
        char vcrun_error[PATH_MAX + 128];
        if (!ms_wine_repair_vcrun(home, prefix, vcrun_error, sizeof(vcrun_error)))
            fprintf(stderr, "warning: %s\n", vcrun_error);
    }
    (void)ensure_dir(fonts);
    report_prefix_progress("Core fonts", "Installing the Microsoft core fonts...");
    for (size_t i = 0; i < sizeof(corefonts) / sizeof(corefonts[0]); i++) {
        char font[PATH_MAX];
        if (!format_path(font, sizeof(font), "%s/corefonts/%s", share, corefonts[i]) || access(font, R_OK) != 0)
            continue;
        {
            const char* const unpack[] = {cabextract, "-q", "-L", "-F", "*.ttf", "-d", fonts, font, NULL};
            (void)run_wait(unpack, NULL, log, 120);
        }
    }
    if (!prefix_file(prefix, ".metalsharp-wine-arch", path, sizeof(path)) || !write_file(path, "arm64\n", 0644) ||
        !prefix_file(prefix, ".metalsharp-prefix-ready", path, sizeof(path)) ||
        !write_file(path, FACADE_VERSION "\n", 0644)) {
        set_error(error, error_size, "could not record the prefix as ready", prefix);
        return false;
    }
    return true;
}

/* The installed MetalSharp-Wine.app swapped for the one MetalSharp.app carries when they differ, in place (only an
 * install that setup finished: a fresh one gets MetalSharp Wine from the setup wizard). The lock keeps two from
 * replacing the app at the same time; wait says whether to wait for it. */
static bool refresh_app(const char* home, bool wait, char* error, size_t error_size) {
    char app[PATH_MAX], installed[256] = "", carried[256] = "";
    char* source = ms_wine_fex_app_source();
    int lock = ms_wine_fex_lock(home, wait);
    bool ok = true;
    if (lock >= 0 && source && ms_wine_app_path(home, app, sizeof(app)) && access(app, F_OK) == 0) {
        bool same = app_identity(app, installed, sizeof(installed)) &&
                    app_identity(source, carried, sizeof(carried)) && !strcmp(installed, carried);
        ok = same ? ms_wine_fex_facade_ready(home) || ms_wine_fex_write_facade(home, error, error_size)
                  : install_app_locked(home, strdup(source), "MetalSharp-Wine.app", error, error_size) &&
                        ms_wine_fex_write_facade(home, error, error_size);
    } else if (lock < 0 && wait) {
        set_error(error, error_size, "could not lock MetalSharp Wine", home);
        ok = false;
    }
    ms_wine_fex_unlock(lock);
    free(source);
    return ok;
}

bool ms_wine_fex_refresh_app(const char* home, char* error, size_t error_size) {
    return home && refresh_app(home, true, error, error_size);
}

/* The startup refreshes (MetalSharp Wine's app here, the x86_64 runtime's in setup.c) run on threads of their own.
 * While one runs, the runtime it lays out is half there, so setup's state waits for them (ms_wine_refresh_wait):
 * otherwise an update that brings a newer Wine app opens the setup wizard as if the runtime needed migrating. */
static pthread_mutex_t refresh_gate_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t refresh_gate_done = PTHREAD_COND_INITIALIZER;
static int refresh_gate_pending;

void ms_wine_refresh_begin(void) {
    pthread_mutex_lock(&refresh_gate_lock);
    refresh_gate_pending++;
    pthread_mutex_unlock(&refresh_gate_lock);
}

void ms_wine_refresh_end(void) {
    pthread_mutex_lock(&refresh_gate_lock);
    if (refresh_gate_pending > 0 && --refresh_gate_pending == 0)
        pthread_cond_broadcast(&refresh_gate_done);
    pthread_mutex_unlock(&refresh_gate_lock);
}

bool ms_wine_refresh_wait(unsigned seconds) {
    struct timespec deadline;
    bool done;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += seconds;
    pthread_mutex_lock(&refresh_gate_lock);
    while (refresh_gate_pending > 0)
        if (pthread_cond_timedwait(&refresh_gate_done, &refresh_gate_lock, &deadline) != 0)
            break;
    done = refresh_gate_pending == 0;
    pthread_mutex_unlock(&refresh_gate_lock);
    return done;
}

static void* refresh_thread(void* argument) {
    char* home = argument;
    char error[512] = "";
    /* A pending or running migration swaps the app itself (ms_wine_fex_refresh_app). */
    if (!ms_wine_fex_refresh_blocked(home) && !refresh_app(home, false, error, sizeof(error)))
        fprintf(stderr, "warning: could not refresh MetalSharp Wine: %s\n", error);
    /* and the x86_64 runtime's app, when it is installed too */
    if (!ms_wine_fex_refresh_blocked(home) && !ms_wine_refresh_inactive_app(home, error, sizeof(error)))
        fprintf(stderr, "warning: could not refresh the other Wine runtime: %s\n", error);
    free(home);
    ms_wine_refresh_end();
    return NULL;
}

void ms_wine_fex_refresh_async(const char* home) {
    pthread_t thread;
    char* copy = home ? strdup(home) : NULL;
    if (!copy)
        return;
    ms_wine_refresh_begin();
    if (pthread_create(&thread, NULL, refresh_thread, copy) != 0) {
        free(copy);
        ms_wine_refresh_end();
        return;
    }
    pthread_detach(thread);
}

/* The fex build's D3D lane is Mullion; the regular build's is Mullion once MetalSharp-Wine-x86_64.app installed it
 * (runtime/wine/lib/dxmt/VERSION, "Mullion <commit> (x86_64)"), DXMT 0.80 from the bundles before. */
bool ms_wine_dxmt_lane_is_mullion(const char* home) {
    char path[PATH_MAX], text[16] = "";
    FILE* file;
    if (ms_wine_is_fex())
        return true;
    if (!home || !format_path(path, sizeof(path), "%s/runtime/%s", home, "wine/lib/dxmt/VERSION") ||
        !(file = fopen(path, "r")))
        return false;
    if (!fgets(text, sizeof(text), file))
        text[0] = '\0';
    fclose(file);
    return !strncmp(text, "Mullion", 7);
}

/* Both builds run Mullion (the x86_64 build since MetalSharp-Wine-x86_64.app), which reads mullion.conf and
 * MULLION_CONFIG_FILE; a dxmt.conf from before becomes it. */
bool ms_wine_graphics_conf_path(const char* home, char* out, size_t out_size) {
    char old[PATH_MAX];
    if (!home || !format_path(out, out_size, "%s/runtime/wine/etc/%s", home, "mullion.conf"))
        return false;
    if (access(out, F_OK) != 0 &&
        format_path(old, sizeof(old), "%s/runtime/wine/etc/%s", home, "dxmt.conf") && access(old, F_OK) == 0)
        (void)rename(old, out);
    return true;
}

const char* ms_wine_graphics_conf_env(void) {
    return "MULLION_CONFIG_FILE";
}

bool ms_wine_vkd3d_lane_conf_path(const char* home, int index, char* out, size_t out_size) {
    return home && (index == 0 || index == 1) &&
           format_path(out, out_size, "%s/runtime/wine/etc/%s", home, index == 0 ? "vkd3d.conf" : "dxvk.conf");
}

#define X86_64_BACKUP ".metalsharp-x86_64-backup"

static bool steam_prefix_arm64(const char* home, bool* x86_64) {
    char prefix[PATH_MAX], registry[PATH_MAX];
    bool arm64 = format_path(prefix, sizeof(prefix), "%s/%s", home, "prefix-steam") && ms_wine_prefix_is_arm64(prefix);
    if (x86_64)
        *x86_64 = !arm64 && format_path(registry, sizeof(registry), "%s/%s", prefix, "system.reg") &&
                  access(registry, F_OK) == 0;
    return arm64;
}

const char* ms_wine_recorded_variant(const char* home, const char* recorded) {
    if (recorded && (!strcmp(recorded, "fex") || !strcmp(recorded, "regular")))
        return !strcmp(recorded, "fex") ? "fex" : "regular";
    return home && steam_prefix_arm64(home, NULL) ? "fex" : "regular";
}

const char* ms_wine_setup_variant_to_record(const char* home) {
    bool x86_64 = false;
    bool arm64 = home && steam_prefix_arm64(home, &x86_64);
    if (!home || (ms_wine_is_fex() ? x86_64 : arm64))
        return NULL;
    return ms_wine_variant();
}

static bool remove_regular_file(const char* dir, const char* name) {
    char path[PATH_MAX];
    struct stat info;
    return format_path(path, sizeof(path), "%s/%s", dir, name) && lstat(path, &info) == 0 &&
           S_ISREG(info.st_mode) && unlink(path) == 0;
}

unsigned ms_wine_fex_remove_old_game_caches(const char* dir, const char* executable) {
    static const char* const vkd3d_names[] = {"vkd3d-proton.cache", "vkd3d-proton.cache.write"};
    char meta[PATH_MAX], marker[PATH_MAX], dxvk_name[NAME_MAX + 1];
    const char* base = executable ? strrchr(executable, '/') : NULL;
    size_t stem;
    unsigned removed = 0;
    if (!dir || !format_path(meta, sizeof(meta), "%s/%s", dir, ".metalsharp") ||
        !format_path(marker, sizeof(marker), "%s/%s", meta, "old-caches-cleared") || access(marker, F_OK) == 0)
        return 0;
    for (size_t i = 0; i < sizeof(vkd3d_names) / sizeof(vkd3d_names[0]); i++)
        removed += remove_regular_file(dir, vkd3d_names[i]);
    /* DXVK names its state cache after the executable without its extension. */
    base = base ? base + 1 : executable;
    if (base && base[0]) {
        stem = strlen(base);
        if (stem > 4 && !strcasecmp(base + stem - 4, ".exe"))
            stem -= 4;
        if (snprintf(dxvk_name, sizeof(dxvk_name), "%.*s.dxvk-cache", (int)stem, base) < (int)sizeof(dxvk_name))
            removed += remove_regular_file(dir, dxvk_name);
    }
    if (ensure_dir(meta))
        (void)write_file(marker, "1\n", 0644);
    return removed;
}

bool ms_wine_prefix_is_arm64(const char* prefix) {
    char path[PATH_MAX], text[16] = "";
    FILE* file;
    if (!prefix || !prefix_file(prefix, ".metalsharp-wine-arch", path, sizeof(path)) || !(file = fopen(path, "rb")))
        return false;
    if (!fgets(text, sizeof(text), file))
        text[0] = '\0';
    fclose(file);
    return !strncmp(text, "arm64", 5);
}

static bool is_prefix(const char* prefix) {
    char path[PATH_MAX];
    return prefix_file(prefix, "system.reg", path, sizeof(path)) && access(path, R_OK) == 0;
}

static const char* const backed_up[] = {"system.reg", "user.reg", "userdef.reg", "drive_c/windows"};

bool ms_wine_fex_convert_prefix(const char* home, const char* prefix, char* error, size_t error_size) {
    char backup[PATH_MAX], done_marker[PATH_MAX];
    if (!is_prefix(prefix))
        return ms_wine_fex_prepare_prefix(home, prefix, false, error, error_size);
    if (ms_wine_prefix_is_arm64(prefix) && ms_wine_fex_prefix_ready(prefix))
        return ms_wine_fex_prepare_prefix(home, prefix, false, error, error_size);
    if (!prefix_file(prefix, X86_64_BACKUP, backup, sizeof(backup)) ||
        !format_path(done_marker, sizeof(done_marker), "%s/%s", backup, ".complete"))
        return false;
    /* A conversion that was interrupted after its backup completed (its prepare failed: MetalSharp Wine's
     * wineboot may already have changed drive_c/windows) keeps that backup, the prefix's only true x86_64
     * copy. A backup is never left over from an earlier move: going back to the regular build restores
     * it and removes it, so a later move to this build always makes a fresh one. */
    if (!ms_wine_prefix_is_arm64(prefix) && access(done_marker, F_OK) != 0) {
        report_prefix_progress("Backup", "Backing up the x86_64 prefix...");
        (void)remove_tree(backup);
        if (!ensure_dir(backup)) {
            set_error(error, error_size, "could not back up the prefix", prefix);
            return false;
        }
        for (size_t i = 0; i < sizeof(backed_up) / sizeof(backed_up[0]); i++) {
            char source[PATH_MAX], target[PATH_MAX];
            if (!prefix_file(prefix, backed_up[i], source, sizeof(source)) ||
                !format_path(target, sizeof(target), "%s/%s", backup, backed_up[i]))
                return false;
            if (access(source, F_OK) == 0) {
                const char* const copy[] = {"/usr/bin/ditto", source, target, NULL};
                if (!run_wait(copy, NULL, NULL, 1800)) {
                    set_error(error, error_size, "could not back up the prefix", source);
                    return false;
                }
            }
        }
        if (!write_file(done_marker, "x86_64\n", 0644))
            return false;
    }
    return ms_wine_fex_prepare_prefix(home, prefix, true, error, error_size);
}

/* Calls visit for each MetalSharp prefix under home; stops at none. */
static unsigned each_prefix(const char* home, bool (*visit)(const char* home, const char* prefix, void* context),
                            void* context) {
    static const char* const fixed[] = {"prefix-steam", "prefix-ubisoft", "prefix-battlenet", "sharp-prefix"};
    char path[PATH_MAX];
    unsigned failed = 0;
    DIR* bottles;
    struct dirent* entry;
    for (size_t i = 0; i < sizeof(fixed) / sizeof(fixed[0]); i++)
        if (format_path(path, sizeof(path), "%s/%s", home, fixed[i]) && is_prefix(path) && !visit(home, path, context))
            failed++;
    if (!format_path(path, sizeof(path), "%s/%s", home, "bottles") || !(bottles = opendir(path)))
        return failed;
    while ((entry = readdir(bottles)) != NULL) {
        char prefix[PATH_MAX];
        if (entry->d_name[0] == '.')
            continue;
        if (snprintf(prefix, sizeof(prefix), "%s/%s/prefix", path, entry->d_name) < (int)sizeof(prefix) &&
            is_prefix(prefix) && !visit(home, prefix, context))
            failed++;
    }
    closedir(bottles);
    return failed;
}

typedef struct {
    char* error;
    size_t error_size;
} convert_context;

static bool convert_visit(const char* home, const char* prefix, void* opaque) {
    convert_context* context = opaque;
    return ms_wine_fex_convert_prefix(home, prefix, context->error, context->error_size);
}

unsigned ms_wine_fex_convert_prefixes(const char* home, char* error, size_t error_size) {
    convert_context context = {error, error_size};
    return home ? each_prefix(home, convert_visit, &context) : 0;
}

typedef struct {
    unsigned without_backup;
} restore_context;

/* Puts the backup's copy of one item in place: copied beside the prefix's own first, then swapped in, so a
 * copy that fails (a full disk) leaves the prefix's item as it was. */
static bool restore_item(const char* source, const char* target) {
    char staged[PATH_MAX], replaced[PATH_MAX];
    struct stat info;
    const char* const copy[] = {"/usr/bin/ditto", source, staged, NULL};
    if (!format_path(staged, sizeof(staged), "%s%s", target, ".metalsharp-restore") ||
        !format_path(replaced, sizeof(replaced), "%s%s", target, ".metalsharp-replaced") || !remove_tree(staged) ||
        !remove_tree(replaced))
        return false;
    if (!run_wait(copy, NULL, NULL, 1800)) {
        (void)remove_tree(staged);
        return false;
    }
    if (lstat(target, &info) == 0 && rename(target, replaced) != 0) {
        (void)remove_tree(staged);
        return false;
    }
    if (rename(staged, target) != 0) {
        (void)rename(replaced, target);
        (void)remove_tree(staged);
        return false;
    }
    (void)remove_tree(replaced);
    return true;
}

/* A prefix with a complete x86_64 backup gets it back, whatever its arch marker says: a conversion whose
 * prepare failed has no marker but MetalSharp Wine may have changed it. The backup is then removed (its
 * .complete marker first), so it is never used again. Returns false when a restore failed, which a later
 * try can fix; an arm64 prefix without a backup is counted apart and left alone. */
static bool restore_visit(const char* home, const char* prefix, void* opaque) {
    restore_context* context = opaque;
    char backup[PATH_MAX], done_marker[PATH_MAX], path[PATH_MAX];
    (void)home;
    if (!prefix_file(prefix, X86_64_BACKUP, backup, sizeof(backup)) ||
        !format_path(done_marker, sizeof(done_marker), "%s/%s", backup, ".complete"))
        return false;
    if (access(done_marker, F_OK) != 0) {
        if (ms_wine_prefix_is_arm64(prefix) && context)
            context->without_backup++;
        return true;
    }
    for (size_t i = 0; i < sizeof(backed_up) / sizeof(backed_up[0]); i++) {
        char source[PATH_MAX], target[PATH_MAX];
        if (!format_path(source, sizeof(source), "%s/%s", backup, backed_up[i]) ||
            !prefix_file(prefix, backed_up[i], target, sizeof(target)))
            return false;
        if (access(source, F_OK) == 0 && !restore_item(source, target))
            return false;
    }
    if (prefix_file(prefix, ".metalsharp-prefix-ready", path, sizeof(path)))
        (void)unlink(path);
    if (prefix_file(prefix, ".metalsharp-wine-arch", path, sizeof(path)))
        (void)unlink(path);
    (void)unlink(done_marker);
    (void)remove_tree(backup);
    return true;
}

unsigned ms_wine_restore_x86_64_prefixes(const char* home, unsigned* without_backup) {
    restore_context context = {0};
    unsigned failed = home ? each_prefix(home, restore_visit, &context) : 0;
    if (without_backup)
        *without_backup = context.without_backup;
    return failed;
}

/* --- Two runtimes in one home ------------------------------------------------------------------------------------
 * MetalSharp-Wine.app (FEX) and MetalSharp-Wine-x86_64.app both install into runtime/, but each lays out its own
 * runtime/wine (the FEX facade, the x86_64 tree), runtime/host and support assets there. The inactive runtime's are
 * kept in runtime/.inactive-<variant>, moved back (renames) when the user switches to it; the apps themselves stay.
 * Prefixes are shared: Wine updates a prefix to the runtime it starts with (wineboot), and what games installed in it
 * stays. */

static bool runtime_stash(const char* home, const char* variant, char* out, size_t out_size) {
    return format_path(out, out_size, "%s/runtime/.inactive-%s", home, variant);
}

/* The entries of runtime/ that belong to the active runtime: all but the Wine apps (and their backups) and dot files */
static bool runtime_entry_switches(const char* name) {
    size_t length = strlen(name);
    return name[0] != '.' && !(length > 4 && !strcmp(name + length - 4, ".app")) && !strstr(name, ".app.");
}

/* runtime/wine as `variant` lays it out, active (in runtime/) or kept aside */
static bool runtime_wine_ready(const char* wine, const char* variant) {
    char path[PATH_MAX];
    if (!strcmp(variant, "fex"))
        return format_path(path, sizeof(path), "%s/%s", wine, FACADE_MARKER) && access(path, R_OK) == 0;
    return format_path(path, sizeof(path), "%s/%s", wine, "lib/wine/x86_64-unix/ntdll.so") && access(path, R_OK) == 0;
}

bool ms_wine_runtime_installed(const char* home, const char* variant) {
    char path[PATH_MAX], wine[PATH_MAX];
    bool fex = variant && !strcmp(variant, "fex");
    if (!home || !variant || (!fex && strcmp(variant, "regular")))
        return false;
    if (fex && !(format_path(path, sizeof(path), "%s/runtime/%s", home, "MetalSharp-Wine.app/Contents/MacOS/wine") &&
                 access(path, X_OK) == 0))
        return false;
    if (fex == ms_wine_is_fex())
        return format_path(wine, sizeof(wine), "%s/runtime/%s", home, "wine") && runtime_wine_ready(wine, variant);
    return runtime_stash(home, variant, path, sizeof(path)) &&
           format_path(wine, sizeof(wine), "%s/%s", path, "wine") && runtime_wine_ready(wine, variant);
}

/* Where the runtime installs from: MetalSharp.app's own, or one downloaded into <home>/downloads */
static bool runtime_source_available(const char* variant) {
    char* source = !strcmp(variant, "fex") ? ms_wine_fex_app_source() : ms_wine_x86_64_app_source();
    bool available = source != NULL;
    free(source);
    return available;
}

static void write_runtime_state(ms_json_writer* writer, const char* home, const char* variant) {
    ms_json_writer_key(writer, variant);
    ms_json_writer_object_begin(writer);
    ms_json_writer_key(writer, "installed");
    ms_json_writer_bool(writer, ms_wine_runtime_installed(home, variant));
    ms_json_writer_key(writer, "source");
    ms_json_writer_bool(writer, runtime_source_available(variant));
    ms_json_writer_key(writer, "label");
    ms_json_writer_string(writer, !strcmp(variant, "fex") ? "FEX" : "x86_64");
    ms_json_writer_object_end(writer);
}

char* ms_wine_runtimes_json(const char* home) {
    ms_json_writer writer;
    ms_json_writer_init(&writer);
    ms_json_writer_object_begin(&writer);
    ms_json_writer_key(&writer, "ok");
    ms_json_writer_bool(&writer, true);
    ms_json_writer_key(&writer, "active");
    ms_json_writer_string(&writer, ms_wine_variant());
    ms_json_writer_key(&writer, "app_variant");
    ms_json_writer_string(&writer, ms_wine_app_variant());
    write_runtime_state(&writer, home, "fex");
    write_runtime_state(&writer, home, "regular");
    ms_json_writer_object_end(&writer);
    return ms_json_writer_take(&writer);
}

/* Stops every Wine the active runtime runs in MetalSharp's prefixes (Steam, launchers, games) */
static bool stop_prefix_visit(const char* home, const char* prefix, void* context) {
    const char* const* server = (const char* const*)context;
    const char* const argv[] = {server[0], "-k", NULL};
    (void)home;
    (void)run_wait(argv, prefix, NULL, 30);
    return true;
}

static bool stop_active_wine(const char* home) {
    char server[PATH_MAX];
    const char* context[1] = {server};
    if (!ms_wine_server_path(home, server, sizeof(server)) || access(server, X_OK) != 0)
        return true;
    (void)each_prefix(home, stop_prefix_visit, (void*)context);
    return true;
}

/* Moves runtime/'s switching entries into dest (out of runtime/), or the entries of src back into runtime/ */
static bool move_runtime_entries(const char* from, const char* to, bool filter, char* error, size_t error_size) {
    DIR* dir = opendir(from);
    struct dirent* entry;
    bool ok = true;
    if (!dir)
        return true;
    if (!ensure_dir(to)) {
        closedir(dir);
        set_error(error, error_size, "could not create", to);
        return false;
    }
    while ((entry = readdir(dir)) != NULL) {
        char source[PATH_MAX], target[PATH_MAX];
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..") ||
            (filter && !runtime_entry_switches(entry->d_name)))
            continue;
        if (!format_path(source, sizeof(source), "%s/%s", from, entry->d_name) ||
            !format_path(target, sizeof(target), "%s/%s", to, entry->d_name))
            continue;
        (void)remove_tree(target);
        if (rename(source, target) != 0) {
            set_error(error, error_size, "could not move", source);
            ok = false;
            break;
        }
    }
    closedir(dir);
    return ok;
}

/* setup.json records the runtime the home is on, so the next start does not take the switch for a migration */
static void record_runtime_variant(const char* home, const char* variant) {
    char path[PATH_MAX], tmp[PATH_MAX];
    ms_json_writer writer;
    ms_json* existing = NULL;
    char* text = NULL;
    FILE* file;
    if (!format_path(path, sizeof(path), "%s/%s", home, "setup.json") || !format_path(tmp, sizeof(tmp), "%s/%s", home, "setup.json.tmp"))
        return;
    if ((file = fopen(path, "rb"))) {
        char buffer[65536];
        size_t length = fread(buffer, 1, sizeof(buffer) - 1, file);
        fclose(file);
        buffer[length] = '\0';
        existing = ms_json_parse(buffer, length, NULL, 0);
    }
    ms_json_writer_init(&writer);
    ms_json_writer_object_begin(&writer);
    if (existing && ms_json_type_of(existing) == MS_JSON_OBJECT)
        for (size_t i = 0; i < ms_json_object_length(existing); i++) {
            const char* key = ms_json_object_key_at(existing, i);
            char* value;
            if (!key || !strcmp(key, "runtime_variant") || !(value = ms_json_stringify(ms_json_object_value_at(existing, i))))
                continue;
            ms_json_writer_key(&writer, key);
            ms_json_writer_raw(&writer, value);
            free(value);
        }
    ms_json_writer_key(&writer, "runtime_variant");
    ms_json_writer_string(&writer, variant);
    ms_json_writer_object_end(&writer);
    text = ms_json_writer_take(&writer);
    ms_json_free(existing);
    if (text && (file = fopen(tmp, "wb"))) {
        bool written = fputs(text, file) >= 0;
        if (fclose(file) == 0 && written)
            (void)rename(tmp, path);
    }
    free(text);
}

bool ms_wine_switch_runtime(const char* home, const char* target, char* error, size_t error_size) {
    char runtime[PATH_MAX], stash_from[PATH_MAX], stash_to[PATH_MAX], config[PATH_MAX], file_path[PATH_MAX];
    const char* active = ms_wine_variant();
    int lock;
    bool ok;
    FILE* file;
    if (!home || !target || (strcmp(target, "fex") && strcmp(target, "regular"))) {
        set_error(error, error_size, "unknown runtime", target);
        return false;
    }
    if (!strcmp(target, active))
        return true;
    if (!format_path(runtime, sizeof(runtime), "%s/%s", home, "runtime") ||
        !runtime_stash(home, active, stash_from, sizeof(stash_from)) ||
        !runtime_stash(home, target, stash_to, sizeof(stash_to)) ||
        !format_path(config, sizeof(config), "%s/%s", home, "config") ||
        !active_runtime_file(home, file_path, sizeof(file_path)))
        return false;
    (void)stop_active_wine(home);
    if ((lock = ms_wine_fex_lock(home, true)) < 0) {
        set_error(error, error_size, "could not lock", runtime);
        return false;
    }
    ok = move_runtime_entries(runtime, stash_from, true, error, error_size) &&
         move_runtime_entries(stash_to, runtime, false, error, error_size);
    if (ok) {
        (void)rmdir(stash_to);
        ok = ensure_dir(config) && (file = fopen(file_path, "w")) && fprintf(file, "%s\n", target) > 0 &&
             fclose(file) == 0;
        if (!ok)
            set_error(error, error_size, "could not record the runtime in", file_path);
    }
    if (!ok) {
        /* put the active runtime back */
        (void)move_runtime_entries(runtime, stash_to, true, NULL, 0);
        (void)move_runtime_entries(stash_from, runtime, false, NULL, 0);
    }
    ms_wine_fex_unlock(lock);
    if (ok) {
        record_runtime_variant(home, target);
        active_fex = !strcmp(target, "fex");
        /* the prefixes' VC++ runtime for the runtime they now start with; the app then calls
         * POST /runtime/vcrun/repair for any prefix still without Microsoft's (an install can outlast this request) */
        (void)ms_wine_fit_prefix_vcrun(home, !active_fex);
    }
    return ok;
}

char* ms_wine_switch_runtime_json(const char* home, const char* body, size_t body_length) {
    char error[PATH_MAX + 128] = "";
    ms_json* request = body && body_length ? ms_json_parse(body, body_length, NULL, 0) : NULL;
    const ms_json* target_value = request ? ms_json_object_get(request, "variant") : NULL;
    char* target = NULL;
    ms_json_writer writer;
    bool ok;
    if (target_value)
        (void)ms_json_as_string(target_value, &target);
    ok = ms_wine_switch_runtime(home, target, error, sizeof(error));
    ms_json_writer_init(&writer);
    ms_json_writer_object_begin(&writer);
    ms_json_writer_key(&writer, "ok");
    ms_json_writer_bool(&writer, ok);
    ms_json_writer_key(&writer, "active");
    ms_json_writer_string(&writer, ms_wine_variant());
    ms_json_writer_key(&writer, "installed");
    ms_json_writer_bool(&writer, ok && ms_wine_runtime_installed(home, ms_wine_variant()));
    /* the backend restarts on the runtime it switched to */
    ms_json_writer_key(&writer, "restart");
    ms_json_writer_bool(&writer, ok);
    if (!ok) {
        ms_json_writer_key(&writer, "error");
        ms_json_writer_string(&writer, error[0] ? error : "could not switch the Wine runtime");
    }
    ms_json_writer_object_end(&writer);
    free(target);
    ms_json_free(request);
    return ms_json_writer_take(&writer);
}

/* --- One runtime uninstalled -------------------------------------------------------------------------------------
 * Settings' "Uninstall FEX" / "Uninstall x86_64": that runtime's Wine app, its laid-out files (active in runtime/ or
 * kept aside), its download and its lanes' caches go; prefixes, which both runtimes share, stay. The active runtime
 * goes only when the other is installed: the switch to it comes first (and the backend restarts on it). The last
 * runtime stays: removing it is "Uninstall MetalSharp". MetalSharp.app still carries the apps, so either runtime can
 * be set up again from the FEX | x86_64 toggle. */
bool ms_wine_uninstall_runtime(const char* home, const char* variant, bool* switched, char* error, size_t error_size) {
    static const char* const fex_caches[] = {"shader-cache/fex-dxmt", "shader-cache/fex-dxmt_32",
                                             "shader-cache/fex-vkd3d", "shader-cache/fex-dxvk",
                                             "pipeline-cache/fex-dxmt", "pipeline-cache/fex-dxmt_32",
                                             "pipeline-cache/fex-vkd3d", "pipeline-cache/fex-dxvk"};
    static const char* const x86_64_caches[] = {
        "shader-cache/dxmt",     "shader-cache/dxmt_32",   "shader-cache/vkd3d",     "shader-cache/dxvk",
        "shader-cache/d3dmetal", "pipeline-cache/dxmt",    "pipeline-cache/dxmt_32", "pipeline-cache/vkd3d",
        "pipeline-cache/dxvk",   "pipeline-cache/d3dmetal"};
    const char* other;
    const char* app;
    const char* const* caches;
    size_t cache_count;
    char path[PATH_MAX];
    int lock;
    bool ok = true;
    if (switched)
        *switched = false;
    if (!home || !variant || (strcmp(variant, "fex") && strcmp(variant, "regular"))) {
        set_error(error, error_size, "unknown runtime", variant);
        return false;
    }
    other = !strcmp(variant, "fex") ? "regular" : "fex";
    app = !strcmp(variant, "fex") ? "MetalSharp-Wine.app" : WINE_X86_64_APP;
    caches = !strcmp(variant, "fex") ? fex_caches : x86_64_caches;
    cache_count = !strcmp(variant, "fex") ? sizeof(fex_caches) / sizeof(fex_caches[0])
                                          : sizeof(x86_64_caches) / sizeof(x86_64_caches[0]);
    if (!strcmp(variant, ms_wine_variant())) {
        if (!ms_wine_runtime_installed(home, other)) {
            set_error(error, error_size, "it is the only Wine runtime installed; Uninstall MetalSharp removes it",
                      variant);
            return false;
        }
        if (!ms_wine_switch_runtime(home, other, error, error_size))
            return false;
        if (switched)
            *switched = true;
    }
    if ((lock = ms_wine_fex_lock(home, true)) < 0) {
        set_error(error, error_size, "could not lock", home);
        return false;
    }
    ok = runtime_stash(home, variant, path, sizeof(path)) && remove_tree(path) &&
         format_path(path, sizeof(path), "%s/runtime/%s", home, app) && remove_tree(path) &&
         format_path(path, sizeof(path), "%s/downloads/%s", home, app) && remove_tree(path);
    ms_wine_fex_unlock(lock);
    if (!ok) {
        set_error(error, error_size, "could not remove the runtime's files", path);
        return false;
    }
    /* its lanes' caches (what is left of a folder that cannot be removed does no harm) */
    for (size_t i = 0; i < cache_count; i++)
        if (format_path(path, sizeof(path), "%s/%s", home, caches[i]))
            (void)remove_tree(path);
    return true;
}

char* ms_wine_uninstall_runtime_json(const char* home, const char* body, size_t body_length) {
    char error[PATH_MAX + 128] = "";
    ms_json* request = body && body_length ? ms_json_parse(body, body_length, NULL, 0) : NULL;
    const ms_json* value = request ? ms_json_object_get(request, "variant") : NULL;
    char* variant = NULL;
    bool switched = false, ok;
    ms_json_writer writer;
    if (value)
        (void)ms_json_as_string(value, &variant);
    ok = ms_wine_uninstall_runtime(home, variant, &switched, error, sizeof(error));
    ms_json_writer_init(&writer);
    ms_json_writer_object_begin(&writer);
    ms_json_writer_key(&writer, "ok");
    ms_json_writer_bool(&writer, ok);
    ms_json_writer_key(&writer, "active");
    ms_json_writer_string(&writer, ms_wine_variant());
    /* the backend restarts on the runtime it switched to */
    ms_json_writer_key(&writer, "restart");
    ms_json_writer_bool(&writer, switched);
    if (!ok) {
        ms_json_writer_key(&writer, "error");
        ms_json_writer_string(&writer, error[0] ? error : "could not uninstall the Wine runtime");
    }
    ms_json_writer_object_end(&writer);
    free(variant);
    ms_json_free(request);
    return ms_json_writer_take(&writer);
}

/* --- The Wine runtime, downloaded -------------------------------------------------------------------------------
 * The Wine runtime a MetalSharp.app does not carry comes from MetalSharp Wine's release: the zip of the signed app
 * (MetalSharp-Wine-<version>-arm64.zip for FEX, -x86_64.zip for the x86_64 runtime; $METALSHARP_RUNTIME_URL_FEX /
 * $METALSHARP_RUNTIME_URL_REGULAR override it), unpacked into <home>/downloads, where setup takes it from
 * (app_source). Never MetalSharp's old runtime bundles. */

#define MS_WINE_RELEASE_VERSION "6.0"
#define RUNTIME_RELEASE_URL "https://github.com/metalsharp/MetalSharp-Wine/releases/download/v" MS_WINE_RELEASE_VERSION "/"

typedef struct {
    pthread_mutex_t lock;
    bool running, done, ok;
    char variant[16];
    char error[256];
    long long received, total;
    char partial[PATH_MAX];
} runtime_download_state;

static runtime_download_state runtime_download = {PTHREAD_MUTEX_INITIALIZER, false, false, false, "", "", 0, 0, ""};

static const char* runtime_app_name(const char* variant) {
    return !strcmp(variant, "fex") ? "MetalSharp-Wine.app" : WINE_X86_64_APP;
}

static bool runtime_download_url(const char* variant, char* out, size_t out_size) {
    const char* configured = getenv(!strcmp(variant, "fex") ? "METALSHARP_RUNTIME_URL_FEX" : "METALSHARP_RUNTIME_URL_REGULAR");
    if (configured && configured[0])
        return snprintf(out, out_size, "%s", configured) < (int)out_size;
    return snprintf(out, out_size, "%sMetalSharp-Wine-%s-%s.zip", RUNTIME_RELEASE_URL, MS_WINE_RELEASE_VERSION,
                    !strcmp(variant, "fex") ? "arm64" : "x86_64") < (int)out_size;
}

/* The runtime's app downloaded, unpacked, its signature checked, and kept at <home>/downloads/<app>. NULL on success,
 * else what failed. partial (optional) gets the zip's path while it downloads, for the progress. */
static const char* fetch_runtime_app(const char* home, const char* variant, void (*partial)(const char* zip)) {
    char url[PATH_MAX], downloads[PATH_MAX], zip[PATH_MAX], unpack[PATH_MAX], app[PATH_MAX], unpacked[PATH_MAX],
        log[PATH_MAX];
    const char* name = runtime_app_name(variant);
    if (!runtime_download_url(variant, url, sizeof(url)) ||
        !format_path(downloads, sizeof(downloads), "%s/%s", home, "downloads") || !ensure_dir(downloads) ||
        !format_path(zip, sizeof(zip), "%s/%s.zip.part", downloads, name) ||
        !format_path(unpack, sizeof(unpack), "%s/.unpack-%s", downloads, variant) ||
        !format_path(app, sizeof(app), "%s/%s", downloads, name) ||
        !format_path(unpacked, sizeof(unpacked), "%s/%s", unpack, name) ||
        !format_path(log, sizeof(log), "%s/%s", home, "logs/runtime-download.log"))
        return "could not prepare the download";
    if (partial)
        partial(zip);
    {
        const char* const fetch[] = {"/usr/bin/curl", "-fL", "--retry", "3", "-o", zip, url, NULL};
        const char* const extract[] = {"/usr/bin/ditto", "-x", "-k", zip, unpack, NULL};
        (void)remove_tree(unpack);
        if (!run_wait(fetch, NULL, log, 3600)) {
            (void)unlink(zip);
            return "could not download the Wine runtime";
        }
        if (!run_wait(extract, NULL, log, 900) || access(unpacked, R_OK) != 0) {
            (void)unlink(zip);
            (void)remove_tree(unpack);
            return "the download is not a Wine runtime";
        }
        (void)unlink(zip);
    }
    if (!app_signature_valid(unpacked, log)) {
        (void)remove_tree(unpack);
        return "the downloaded Wine runtime's signature did not verify";
    }
    if (!remove_tree(app) || rename(unpacked, app) != 0) {
        (void)remove_tree(unpack);
        return "could not keep the downloaded Wine runtime";
    }
    (void)remove_tree(unpack);
    return NULL;
}

bool ms_wine_fetch_runtime_app(const char* home, const char* variant, char* error, size_t error_size) {
    const char* failure = home && variant ? fetch_runtime_app(home, variant, NULL) : "no runtime";
    if (failure)
        set_error(error, error_size, failure, NULL);
    return failure == NULL;
}

static void runtime_download_partial(const char* zip) {
    pthread_mutex_lock(&runtime_download.lock);
    snprintf(runtime_download.partial, sizeof(runtime_download.partial), "%s", zip);
    pthread_mutex_unlock(&runtime_download.lock);
}

static void* runtime_download_worker(void* arg) {
    char* home = arg;
    char variant[16], url[PATH_MAX];
    const char* failure;
    pthread_mutex_lock(&runtime_download.lock);
    snprintf(variant, sizeof(variant), "%s", runtime_download.variant);
    pthread_mutex_unlock(&runtime_download.lock);
    if (runtime_download_url(variant, url, sizeof(url))) {
        /* its size, for the progress */
        char command[PATH_MAX * 2];
        FILE* pipe;
        snprintf(command, sizeof(command), "/usr/bin/curl -sIL '%s' | /usr/bin/awk 'tolower($1)==\"content-length:\"{n=$2} END{print n+0}'", url);
        if ((pipe = popen(command, "r"))) {
            long long total = 0;
            if (fscanf(pipe, "%lld", &total) == 1) {
                pthread_mutex_lock(&runtime_download.lock);
                runtime_download.total = total;
                pthread_mutex_unlock(&runtime_download.lock);
            }
            pclose(pipe);
        }
    }
    failure = fetch_runtime_app(home, variant, runtime_download_partial);
    free(home);
    pthread_mutex_lock(&runtime_download.lock);
    snprintf(runtime_download.error, sizeof(runtime_download.error), "%s", failure ? failure : "");
    runtime_download.ok = failure == NULL;
    runtime_download.done = true;
    runtime_download.running = false;
    pthread_mutex_unlock(&runtime_download.lock);
    return NULL;
}

char* ms_wine_runtime_download_json(const char* home, const char* body, size_t body_length) {
    ms_json* request = body && body_length ? ms_json_parse(body, body_length, NULL, 0) : NULL;
    const ms_json* value = request ? ms_json_object_get(request, "variant") : NULL;
    char* variant = NULL;
    const char* error = NULL;
    pthread_t thread;
    char* home_copy;
    if (value)
        (void)ms_json_as_string(value, &variant);
    if (!home || !variant || (strcmp(variant, "fex") && strcmp(variant, "regular")))
        error = "unknown runtime";
    else if (runtime_source_available(variant))
        error = NULL; /* nothing to download */
    else {
        pthread_mutex_lock(&runtime_download.lock);
        if (runtime_download.running)
            error = "a download is already running";
        else {
            runtime_download.running = true;
            runtime_download.done = runtime_download.ok = false;
            runtime_download.received = runtime_download.total = 0;
            runtime_download.error[0] = runtime_download.partial[0] = '\0';
            snprintf(runtime_download.variant, sizeof(runtime_download.variant), "%s", variant);
        }
        pthread_mutex_unlock(&runtime_download.lock);
        if (!error && (!(home_copy = strdup(home)) || pthread_create(&thread, NULL, runtime_download_worker, home_copy) != 0)) {
            pthread_mutex_lock(&runtime_download.lock);
            runtime_download.running = false;
            pthread_mutex_unlock(&runtime_download.lock);
            error = "could not start the download";
        } else if (!error)
            pthread_detach(thread);
    }
    {
        ms_json_writer writer;
        ms_json_writer_init(&writer);
        ms_json_writer_object_begin(&writer);
        ms_json_writer_key(&writer, "ok");
        ms_json_writer_bool(&writer, error == NULL);
        ms_json_writer_key(&writer, "downloading");
        ms_json_writer_bool(&writer, error == NULL && variant && !runtime_source_available(variant));
        if (error) {
            ms_json_writer_key(&writer, "error");
            ms_json_writer_string(&writer, error);
        }
        ms_json_writer_object_end(&writer);
        free(variant);
        ms_json_free(request);
        return ms_json_writer_take(&writer);
    }
}

char* ms_wine_runtime_download_progress_json(void) {
    ms_json_writer writer;
    struct stat info;
    pthread_mutex_lock(&runtime_download.lock);
    if (runtime_download.running && runtime_download.partial[0] && stat(runtime_download.partial, &info) == 0)
        runtime_download.received = (long long)info.st_size;
    ms_json_writer_init(&writer);
    ms_json_writer_object_begin(&writer);
    ms_json_writer_key(&writer, "running");
    ms_json_writer_bool(&writer, runtime_download.running);
    ms_json_writer_key(&writer, "done");
    ms_json_writer_bool(&writer, runtime_download.done);
    ms_json_writer_key(&writer, "ok");
    ms_json_writer_bool(&writer, runtime_download.ok);
    ms_json_writer_key(&writer, "variant");
    ms_json_writer_string(&writer, runtime_download.variant);
    ms_json_writer_key(&writer, "received");
    ms_json_writer_u64(&writer, (unsigned long long)(runtime_download.received > 0 ? runtime_download.received : 0));
    ms_json_writer_key(&writer, "total");
    ms_json_writer_u64(&writer, (unsigned long long)(runtime_download.total > 0 ? runtime_download.total : 0));
    if (runtime_download.error[0]) {
        ms_json_writer_key(&writer, "error");
        ms_json_writer_string(&writer, runtime_download.error);
    }
    ms_json_writer_object_end(&writer);
    pthread_mutex_unlock(&runtime_download.lock);
    return ms_json_writer_take(&writer);
}

/* --- The VC++ runtime in a prefix shared by both runtimes ---------------------------------------------------------
 * The VC++ 2015-2022 x64 installer puts ARM64X DLLs in System32 when it runs under FEX (it sees ARM64 Windows), and
 * the x86_64 Wine cannot run them: under it they take Microsoft's x64 ones, which MetalSharp-Wine-x86_64.app
 * carries (share/metalsharp/redist/vcrun2022-x64), and get theirs back under MetalSharp Wine, kept aside in
 * <prefix>/.metalsharp-vcrun-arm64. */

#define VCRUN_ARM64_KEPT ".metalsharp-vcrun-arm64"

/* Whether a DLL holds ARM64 code: an ARM64 image, or an ARM64EC/ARM64X one (x64 machine in its header, hybrid
 * metadata in its load configuration), which the VC++ installer puts in under FEX */
static bool pe_has_arm64_code(const char* path) {
    unsigned char dos[0x40], *headers = NULL;
    FILE* file = fopen(path, "rb");
    bool arm64 = false;
    long size;
    if (!file)
        return false;
    if (fseek(file, 0, SEEK_END) == 0 && (size = ftell(file)) > 0x40 && fseek(file, 0, SEEK_SET) == 0 &&
        fread(dos, 1, sizeof(dos), file) == sizeof(dos) && dos[0] == 'M' && dos[1] == 'Z') {
        unsigned pe = dos[0x3c] | dos[0x3d] << 8 | dos[0x3e] << 16 | (unsigned)dos[0x3f] << 24;
        size_t want = 4096;
        if (pe < 4096 - 0x200 && (headers = malloc(want)) && fseek(file, 0, SEEK_SET) == 0 &&
            fread(headers, 1, want, file) == want && headers[pe] == 'P' && headers[pe + 1] == 'E') {
            unsigned machine = headers[pe + 4] | headers[pe + 5] << 8;
            unsigned sections = headers[pe + 6] | headers[pe + 7] << 8;
            unsigned optional_size = headers[pe + 20] | headers[pe + 21] << 8;
            unsigned char* optional = headers + pe + 24;
            if (machine == 0xaa64)
                arm64 = true;
            else if (machine == 0x8664 && optional[0] == 0x0b && optional[1] == 0x02 && optional_size >= 112 + 11 * 8 &&
                     pe + 24 + optional_size + sections * 40 <= want) {
                /* PE32+: data directory 10 (load config) */
                unsigned char* dir = optional + 112 + 10 * 8;
                unsigned rva = dir[0] | dir[1] << 8 | dir[2] << 16 | (unsigned)dir[3] << 24;
                unsigned char* section = optional + optional_size;
                for (unsigned i = 0; rva && i < sections; i++, section += 40) {
                    unsigned vaddr = section[12] | section[13] << 8 | section[14] << 16 | (unsigned)section[15] << 24;
                    unsigned vsize = section[8] | section[9] << 8 | section[10] << 16 | (unsigned)section[11] << 24;
                    unsigned raw = section[20] | section[21] << 8 | section[22] << 16 | (unsigned)section[23] << 24;
                    if (rva >= vaddr && rva < vaddr + vsize) {
                        unsigned char config[0xd0];
                        /* IMAGE_LOAD_CONFIG_DIRECTORY64: Size, then CHPEMetadataPointer at 0xc8 */
                        if (fseek(file, (long)(raw + (rva - vaddr)), SEEK_SET) == 0 &&
                            fread(config, 1, sizeof(config), file) == sizeof(config)) {
                            unsigned config_size = config[0] | config[1] << 8 | config[2] << 16 | (unsigned)config[3] << 24;
                            unsigned long long chpe = 0;
                            for (int b = 7; b >= 0; b--)
                                chpe = chpe << 8 | config[0xc8 + b];
                            arm64 = config_size >= 0xd0 && chpe != 0;
                        }
                        break;
                    }
                }
            }
        }
    }
    free(headers);
    fclose(file);
    return arm64;
}

static bool copy_file_to(const char* source, const char* target) {
    char temporary[PATH_MAX];
    FILE *in, *out;
    char buffer[65536];
    size_t read_bytes;
    bool ok = false;
    if (snprintf(temporary, sizeof(temporary), "%s.metalsharp-tmp", target) >= (int)sizeof(temporary) ||
        !(in = fopen(source, "rb")))
        return false;
    if ((out = fopen(temporary, "wb"))) {
        ok = true;
        while (ok && (read_bytes = fread(buffer, 1, sizeof(buffer), in)) > 0)
            ok = fwrite(buffer, 1, read_bytes, out) == read_bytes;
        ok = fclose(out) == 0 && ok && rename(temporary, target) == 0;
        if (!ok)
            (void)unlink(temporary);
    }
    fclose(in);
    return ok;
}

typedef struct {
    char x64[PATH_MAX];
    unsigned changed;
} vcrun_context;

/* under the x86_64 runtime: each ARM64 (or Wine) VC++ DLL in System32 aside, Microsoft's x64 one in its place */
static bool vcrun_x64_visit(const char* home, const char* prefix, void* opaque) {
    vcrun_context* context = opaque;
    char system32[PATH_MAX], kept[PATH_MAX];
    DIR* dir;
    struct dirent* entry;
    (void)home;
    if (!format_path(system32, sizeof(system32), "%s/%s", prefix, "drive_c/windows/system32") ||
        !format_path(kept, sizeof(kept), "%s/%s", prefix, VCRUN_ARM64_KEPT) || !(dir = opendir(context->x64)))
        return true;
    while ((entry = readdir(dir)) != NULL) {
        char source[PATH_MAX], target[PATH_MAX], aside[PATH_MAX];
        if (entry->d_name[0] == '.' || !strstr(entry->d_name, ".dll") ||
            !format_path(source, sizeof(source), "%s/%s", context->x64, entry->d_name) ||
            !format_path(target, sizeof(target), "%s/%s", system32, entry->d_name) ||
            !format_path(aside, sizeof(aside), "%s/%s", kept, entry->d_name))
            continue;
        /* Microsoft's x64 one already there; else an ARM64 or Wine copy (kept aside) or none at all */
        if (!pe_has_arm64_code(target) && microsoft_pe(target))
            continue;
        if (access(target, F_OK) == 0 &&
            (!ensure_dir(kept) || (access(aside, F_OK) != 0 && rename(target, aside) != 0)))
            continue;
        if (copy_file_to(source, target))
            context->changed++;
    }
    closedir(dir);
    return true;
}

/* under MetalSharp Wine: the prefix's own ARM64X VC++ DLLs back */
static bool vcrun_arm64_visit(const char* home, const char* prefix, void* opaque) {
    vcrun_context* context = opaque;
    char system32[PATH_MAX], kept[PATH_MAX];
    DIR* dir;
    struct dirent* entry;
    (void)home;
    if (!format_path(system32, sizeof(system32), "%s/%s", prefix, "drive_c/windows/system32") ||
        !format_path(kept, sizeof(kept), "%s/%s", prefix, VCRUN_ARM64_KEPT) || !(dir = opendir(kept)))
        return true;
    while ((entry = readdir(dir)) != NULL) {
        char aside[PATH_MAX], target[PATH_MAX];
        if (entry->d_name[0] == '.' || !format_path(aside, sizeof(aside), "%s/%s", kept, entry->d_name) ||
            !format_path(target, sizeof(target), "%s/%s", system32, entry->d_name))
            continue;
        if (rename(aside, target) == 0)
            context->changed++;
    }
    closedir(dir);
    (void)rmdir(kept);
    return true;
}

unsigned ms_wine_fit_prefix_vcrun(const char* home, bool x86_64) {
    vcrun_context context = {"", 0};
    char contents[PATH_MAX];
    if (!home)
        return 0;
    if (x86_64) {
        if (!ms_wine_x86_64_app_contents(home, contents, sizeof(contents)) ||
            !format_path(context.x64, sizeof(context.x64), "%s/%s", contents, "share/metalsharp/redist/vcrun2022-x64") ||
            access(context.x64, R_OK) != 0)
            return 0;
        (void)each_prefix(home, vcrun_x64_visit, &context);
    } else
        (void)each_prefix(home, vcrun_arm64_visit, &context);
    return context.changed;
}

static bool repair_vcrun_visit(const char* home, const char* prefix, void* opaque) {
    char error[PATH_MAX + 128] = "";
    bool ok = ms_wine_repair_vcrun(home, prefix, error, sizeof(error));
    if (!ok) {
        fprintf(stderr, "warning: %s\n", error);
        if (opaque && !((char*)opaque)[0])
            snprintf((char*)opaque, PATH_MAX + 128, "%s", error);
    }
    return ok;
}

unsigned ms_wine_repair_prefixes_vcrun(const char* home, char* error, size_t error_size) {
    char first[PATH_MAX + 128] = "";
    unsigned failed = home ? each_prefix(home, repair_vcrun_visit, first) : 0;
    if (failed && error && error_size)
        snprintf(error, error_size, "%s", first);
    return failed;
}

/* POST /runtime/vcrun/repair: every prefix's VC++ runtime fitted to the active Wine runtime */
char* ms_wine_repair_vcrun_json(const char* home) {
    char error[PATH_MAX + 128] = "";
    unsigned failed;
    ms_json_writer writer;
    if (!home)
        return NULL;
    (void)ms_wine_fit_prefix_vcrun(home, !ms_wine_is_fex());
    failed = ms_wine_repair_prefixes_vcrun(home, error, sizeof(error));
    ms_json_writer_init(&writer);
    ms_json_writer_object_begin(&writer);
    ms_json_writer_key(&writer, "ok");
    ms_json_writer_bool(&writer, failed == 0);
    ms_json_writer_key(&writer, "runtime");
    ms_json_writer_string(&writer, ms_wine_variant());
    if (failed) {
        ms_json_writer_key(&writer, "error");
        ms_json_writer_string(&writer, error);
    }
    ms_json_writer_object_end(&writer);
    return ms_json_writer_take(&writer);
}

/* Whether runtime/'s MetalSharp-Wine-x86_64.app is the one MetalSharp.app carries (same CDHash); true without one */
bool ms_wine_x86_64_app_current(const char* home) {
    char* source = ms_wine_x86_64_app_source();
    char app[PATH_MAX], source_identity[256] = "", installed_identity[256] = "";
    bool current = true;
    if (source && home && format_path(app, sizeof(app), "%s/runtime/%s", home, WINE_X86_64_APP))
        current = app_identity(source, source_identity, sizeof(source_identity)) &&
                  app_identity(app, installed_identity, sizeof(installed_identity)) &&
                  !strcmp(source_identity, installed_identity);
    free(source);
    return current;
}

/* The installed MetalSharp-Wine-x86_64.app's CDHash line (what setup records it laid runtime/ out from) */
bool ms_wine_x86_64_app_identity(const char* home, char* out, size_t out_size) {
    char app[PATH_MAX];
    return home && out && format_path(app, sizeof(app), "%s/runtime/%s", home, WINE_X86_64_APP) &&
           app_identity(app, out, out_size);
}
