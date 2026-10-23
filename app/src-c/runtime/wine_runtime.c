#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE
#endif
#include "metalsharp_backend/wine_runtime.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
#define FACADE_VERSION "1"

const char* ms_wine_variant(void) {
    return ms_wine_is_fex() ? "fex" : "regular";
}

bool ms_wine_is_fex(void) {
    return !strcmp(MS_BACKEND_VARIANT, "fex");
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
    if (!ms_wine_is_fex() || !pipeline)
        return pipeline;
    if (!strcmp(pipeline, "d3dmetal") || !strcmp(pipeline, "vkd3d") || !strcmp(pipeline, "m13"))
        return "dxmt";
    return pipeline;
}

const char* ms_wine_fex_graphics_backend(const char* pipeline) {
    if (pipeline && (!strcmp(pipeline, "m32") || !strcmp(pipeline, "wine_bare")))
        return "wine";
    if (pipeline && !strcmp(pipeline, "dxvk"))
        return "dxvk";
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
    "# GRAPHICS_BACKEND is dxmt (Mullion), dxvk or wine here; d3dmetal would mean wined3d.\n"
    "case \"${GRAPHICS_BACKEND:-}\" in\n"
    "  dxmt|dxvk|wine) ;;\n"
    "  '') unset GRAPHICS_BACKEND ;;\n"
    "  *) export GRAPHICS_BACKEND=dxmt ;;\n"
    "esac\n";

static const char facade_metalsharp_wine[] =
    "case \"${1:-}\" in\n"
    "  steam)\n"
    "    shift\n"
    "    export GRAPHICS_BACKEND=dxmt\n"
    "    export WINEDLLOVERRIDES=\"bcrypt=b;ncrypt=b;gameoverlayrenderer,gameoverlayrenderer64=d\"\n"
    "    exec \"$MS_WINE\" \"$WINEPREFIX/drive_c/Program Files (x86)/Steam/steam.exe\" \\\n"
    "      -no-cef-sandbox -cef-single-process -noverifyfiles -no-dwrite \"$@\"\n"
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

char* ms_wine_fex_app_source(void) {
    const char* override = getenv("METALSHARP_WINE_APP_SOURCE");
    char path[PATH_MAX], resolved[PATH_MAX], candidate[PATH_MAX];
    if (override && override[0])
        return access(override, R_OK) == 0 ? strdup(override) : NULL;
#ifdef __APPLE__
    {
        uint32_t size = sizeof(path);
        /* The backend is MetalSharp.app/Contents/Resources/runtime/metalsharp-backend. */
        if (_NSGetExecutablePath(path, &size) == 0 && realpath(path, resolved)) {
            char* slash = strrchr(resolved, '/');
            if (slash) {
                *slash = '\0';
                if (format_path(candidate, sizeof(candidate), "%s/%s", resolved, "../MetalSharp-Wine.app") &&
                    realpath(candidate, path) && access(path, R_OK) == 0)
                    return strdup(path);
            }
        }
    }
#endif
    (void)resolved;
    snprintf(candidate, sizeof(candidate), "%s", "/Applications/MetalSharp.app/Contents/Resources/MetalSharp-Wine.app");
    return access(candidate, R_OK) == 0 ? strdup(candidate) : NULL;
}

/* CFBundleVersion and the code directory hash identify a MetalSharp Wine build. */
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

bool ms_wine_fex_install_app(const char* home, char* error, size_t error_size) {
    char app[PATH_MAX], staging[PATH_MAX], old[PATH_MAX], runtime[PATH_MAX], log[PATH_MAX], logs[PATH_MAX];
    char source_identity[256] = "", installed_identity[256] = "";
    char* source = ms_wine_fex_app_source();
    bool ok = false;
    if (!source) {
        set_error(error, error_size, "this MetalSharp does not carry MetalSharp-Wine.app; reinstall MetalSharp", NULL);
        return false;
    }
    if (!ms_wine_app_path(home, app, sizeof(app)) || !format_path(staging, sizeof(staging), "%s%s", app, ".new") ||
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
        set_error(error, error_size, "MetalSharp-Wine.app is not signed", source);
        goto done;
    }
    if (!(access(app, R_OK) == 0 && app_identity(app, installed_identity, sizeof(installed_identity)) &&
          !strcmp(source_identity, installed_identity) && app_signature_valid(app, log))) {
        const char* const copy[] = {"/usr/bin/ditto", source, staging, NULL};
        if (!remove_tree(staging) || !run_wait(copy, NULL, log, 900)) {
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
    ok = ms_wine_fex_write_facade(home, error, error_size);
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
    if (!force && ms_wine_fex_prefix_ready(prefix))
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
    (void)ensure_dir(prefix);
    {
        const char* const boot[] = {wine, "wineboot", "-u", NULL};
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
            /* 3010 (reboot required) also counts as installed; the DLL check below decides. */
            (void)run_wait(install, prefix, log, 900);
            (void)run_wait(wait_server, prefix, log, 300);
        }
    }
    if (!vcpp_registered(prefix)) {
        set_error(error, error_size, "the VC++ 2022 runtime did not install; see logs/prefix-setup.log", prefix);
        return false;
    }
    (void)ensure_dir(fonts);
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

static void* refresh_thread(void* argument) {
    char* home = argument;
    char app[PATH_MAX], error[512] = "", installed[256] = "", carried[256] = "";
    char* source = ms_wine_fex_app_source();
    /* Only an install that setup finished: a fresh one gets MetalSharp Wine from the setup wizard. */
    if (source && ms_wine_app_path(home, app, sizeof(app)) && access(app, F_OK) == 0) {
        bool same = app_identity(app, installed, sizeof(installed)) &&
                    app_identity(source, carried, sizeof(carried)) && !strcmp(installed, carried);
        if (same ? !ms_wine_fex_facade_ready(home) && !ms_wine_fex_write_facade(home, error, sizeof(error))
                 : !ms_wine_fex_install_app(home, error, sizeof(error)))
            fprintf(stderr, "warning: could not refresh MetalSharp Wine: %s\n", error);
    }
    free(source);
    free(home);
    return NULL;
}

void ms_wine_fex_refresh_async(const char* home) {
    pthread_t thread;
    char* copy = home ? strdup(home) : NULL;
    if (!copy)
        return;
    if (pthread_create(&thread, NULL, refresh_thread, copy) != 0) {
        free(copy);
        return;
    }
    pthread_detach(thread);
}

#define X86_64_BACKUP ".metalsharp-x86_64-backup"

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

static bool convert_prefix(const char* home, const char* prefix, char* error, size_t error_size) {
    char backup[PATH_MAX], done_marker[PATH_MAX];
    if (!is_prefix(prefix) || (ms_wine_prefix_is_arm64(prefix) && ms_wine_fex_prefix_ready(prefix)))
        return true;
    if (!prefix_file(prefix, X86_64_BACKUP, backup, sizeof(backup)) ||
        !format_path(done_marker, sizeof(done_marker), "%s/%s", backup, ".complete"))
        return false;
    /* An earlier, interrupted conversion keeps the backup it completed. */
    if (!ms_wine_prefix_is_arm64(prefix) && access(done_marker, F_OK) != 0) {
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
    static const char* const fixed[] = {"prefix-steam", "prefix-ubisoft", "sharp-prefix"};
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
    return convert_prefix(home, prefix, context->error, context->error_size);
}

unsigned ms_wine_fex_convert_prefixes(const char* home, char* error, size_t error_size) {
    convert_context context = {error, error_size};
    return home ? each_prefix(home, convert_visit, &context) : 0;
}

static bool restore_visit(const char* home, const char* prefix, void* opaque) {
    char backup[PATH_MAX], done_marker[PATH_MAX], path[PATH_MAX];
    (void)home;
    (void)opaque;
    if (!ms_wine_prefix_is_arm64(prefix))
        return true;
    if (!prefix_file(prefix, X86_64_BACKUP, backup, sizeof(backup)) ||
        !format_path(done_marker, sizeof(done_marker), "%s/%s", backup, ".complete") || access(done_marker, F_OK) != 0)
        return false;
    for (size_t i = 0; i < sizeof(backed_up) / sizeof(backed_up[0]); i++) {
        char source[PATH_MAX], target[PATH_MAX];
        if (!format_path(source, sizeof(source), "%s/%s", backup, backed_up[i]) ||
            !prefix_file(prefix, backed_up[i], target, sizeof(target)))
            return false;
        if (access(source, F_OK) == 0) {
            const char* const copy[] = {"/usr/bin/ditto", source, target, NULL};
            if (!remove_tree(target) || !run_wait(copy, NULL, NULL, 1800))
                return false;
        }
    }
    if (prefix_file(prefix, ".metalsharp-prefix-ready", path, sizeof(path)))
        (void)unlink(path);
    if (prefix_file(prefix, ".metalsharp-wine-arch", path, sizeof(path)))
        (void)unlink(path);
    (void)remove_tree(backup);
    return true;
}

unsigned ms_wine_restore_x86_64_prefixes(const char* home) {
    return home ? each_prefix(home, restore_visit, NULL) : 0;
}
