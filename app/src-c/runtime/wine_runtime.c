#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE
#endif
#include "metalsharp_backend/wine_runtime.h"
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

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
    "  D3DMETAL_RUNTIME_DIR ROSETTA_ADVERTISE_AVX ROSETTA_X87_PATH\n"
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
    char contents[PATH_MAX], root[PATH_MAX], kept[PATH_MAX], bin[PATH_MAX], etc[PATH_MAX], lib[PATH_MAX],
        target[PATH_MAX], marker[PATH_MAX];
    char* prologue;
    size_t prologue_size;
    struct stat info;
    bool ok = true;
    if (!ms_wine_app_contents(home, contents, sizeof(contents)) ||
        !format_path(root, sizeof(root), "%s/runtime/%s", home, "wine") ||
        !format_path(kept, sizeof(kept), "%s/runtime/%s", home, "wine-x86_64") ||
        !format_path(bin, sizeof(bin), "%s/%s", root, "bin") || !format_path(etc, sizeof(etc), "%s/%s", root, "etc") ||
        !format_path(lib, sizeof(lib), "%s/%s", root, "lib") ||
        !format_path(target, sizeof(target), "%s/%s", contents, "lib") ||
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
    if (lstat(lib, &info) == 0 && (!S_ISLNK(info.st_mode) || unlink(lib) != 0)) {
        set_error(error, error_size, "runtime/wine/lib is not a link to MetalSharp Wine", lib);
        return false;
    }
    if (symlink(target, lib) != 0) {
        set_error(error, error_size, "could not link", lib);
        return false;
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
