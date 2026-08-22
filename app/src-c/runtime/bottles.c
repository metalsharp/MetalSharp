#include "metalsharp_backend/bottles.h"
#include "metalsharp_backend/json.h"
#include "metalsharp_backend/json_writer.h"
#include <dirent.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static char* path_join(const char* a, const char* b) {
    size_t x = strlen(a), y = strlen(b);
    int slash = x && a[x - 1] != '/' ? 1 : 0;
    char* p = malloc(x + y + slash + 1);
    if (p)
        snprintf(p, x + y + slash + 1, "%s%s%s", a, slash ? "/" : "", b);
    return p;
}
static char* read_text(const char* p) {
    FILE* f = fopen(p, "rb");
    long n;
    char* s;
    size_t got;
    if (!f || fseek(f, 0, SEEK_END) != 0) {
        if (f)
            fclose(f);
        return NULL;
    }
    n = ftell(f);
    if (n < 0 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }
    s = malloc((size_t)n + 1);
    if (!s) {
        fclose(f);
        return NULL;
    }
    got = fread(s, 1, (size_t)n, f);
    fclose(f);
    s[got] = 0;
    return s;
}
static void obj_string(ms_json_writer* w, const char* k, const char* v) {
    ms_json_writer_key(w, k);
    ms_json_writer_string(w, v);
}
char* ms_bottles_list_json(const char* home) {
    char *root = path_join(home, "bottles"), *p, *raw, *o;
    DIR* d;
    struct dirent* e;
    struct stat st;
    ms_json_writer w;
    ms_json_writer_init(&w);
    ms_json_writer_object_begin(&w);
    ms_json_writer_key(&w, "ok");
    ms_json_writer_bool(&w, true);
    ms_json_writer_key(&w, "bottles");
    ms_json_writer_array_begin(&w);
    d = root ? opendir(root) : NULL;
    if (d) {
        while ((e = readdir(d)) != NULL) {
            if (e->d_name[0] == '.')
                continue;
            p = path_join(root, e->d_name);
            if (!p)
                continue;
            if (stat(p, &st) == 0 && S_ISDIR(st.st_mode)) {
                char* m = path_join(p, "bottle.json");
                raw = m ? read_text(m) : NULL;
                if (raw) {
                    char er[96];
                    ms_json* j = ms_json_parse(raw, strlen(raw), er, sizeof(er));
                    if (j && ms_json_type_of(j) == MS_JSON_OBJECT)
                        ms_json_writer_raw(&w, raw);
                    ms_json_free(j);
                    free(raw);
                }
                free(m);
            }
            free(p);
        }
        closedir(d);
    }
    ms_json_writer_array_end(&w);
    ms_json_writer_object_end(&w);
    o = ms_json_writer_take(&w);
    free(root);
    return o;
}
typedef struct profile {
    const char *id, *name, *arch, *pipeline;
    bool wineboot;
} profile;
static const char** profile_components(const char* id) {
    static const char *plain[] = {NULL}, *launcher[] = {"gecko", "vcrun2019_x64", "vcrun2019_x86", "corefonts", NULL},
                      *game[] = {"vcrun2019_x64", "vcrun2019_x86", "vcrun2013", "directx_jun2010", "corefonts", NULL},
                      *m9[] = {"d3d9", "vcrun2019_x64", "vcrun2019_x86", "directx_jun2010", NULL},
                      *m10[] = {"d3d10", "d3d10_1", "dxgi", "vcrun2019_x64", "vcrun2019_x86", NULL},
                      *m10_32[] = {"d3d10core", "d3d10_1", "winemetal", "vcrun2019_x86", NULL},
                      *m11[] = {"d3d11", "dxgi", "vcrun2019_x64", "vcrun2019_x86", NULL},
                      *m11_32[] = {"d3d11", "dxgi", "winemetal", "vcrun2019_x86", NULL},
                      *m12[] = {"m12_d3d12",     "m12_d3d11",     "m12_d3d10core", "m12_dxgi_dxmt",
                                "m12_dxgi",      "m12_winemetal", "m12_gpu_stubs", "vcrun2019_x64",
                                "vcrun2019_x86", "d3d12_agility", "corefonts",     NULL},
                      *vkd3d[] = {"vkd3d_d3d12",    "vkd3d_d3d12core", "vkd3d_dxgi",    "dxvk_d3d9", "dxvk_d3d11",
                                  "dxvk_d3d10core", "vcrun2019_x64",   "vcrun2019_x86", "corefonts", NULL},
                      *m13[] = {"d3d11", "d3d12", "dxgi", "d3d10", "vcrun2019_x64", "vcrun2019_x86", "gpu_vendor_stubs",
                                NULL},
                      *d3dmetal[] = {"gptk", "rosetta", "gptk_prefix", "vcrun2019_x64", "vcrun2019_x86", NULL},
                      *dotnet[] = {"wine-mono",     "gecko",     "dotnet48", "vcrun2019_x64",
                                   "vcrun2019_x86", "corefonts", NULL},
                      *webview[] = {"gecko",         "webview2",      "dotnet48",
                                    "vcrun2019_x64", "vcrun2019_x86", "directx_jun2010",
                                    "openal",        "corefonts",     NULL},
                      *java_launcher[] = {"vcrun2019_x64", "vcrun2019_x86", "corefonts", NULL},
                      *fna_arm64[] = {"mono-arm64", "fna", "xna", "sdl2", "fna3d", "faudio", NULL},
                      *fna_x86[] = {"mono-x86", "fna", "xna", "sdl2", "fna3d", "faudio", "fmod", NULL};
    if (!strcmp(id, "launcher"))
        return launcher;
    if (!strcmp(id, "game_install"))
        return game;
    if (!strcmp(id, "m9"))
        return m9;
    if (!strcmp(id, "m10"))
        return m10;
    if (!strcmp(id, "m10_32"))
        return m10_32;
    if (!strcmp(id, "m11"))
        return m11;
    if (!strcmp(id, "m11_32"))
        return m11_32;
    if (!strcmp(id, "m12"))
        return m12;
    if (!strcmp(id, "vkd3d"))
        return vkd3d;
    if (!strcmp(id, "m13"))
        return m13;
    if (!strcmp(id, "d3dmetal"))
        return d3dmetal;
    if (!strcmp(id, "dotnet") || !strcmp(id, "win32_dotnet"))
        return dotnet;
    if (!strcmp(id, "webview"))
        return webview;
    if (!strcmp(id, "java_launcher"))
        return java_launcher;
    if (!strcmp(id, "fna_arm64"))
        return fna_arm64;
    if (!strcmp(id, "fna_x86"))
        return fna_x86;
    return plain;
}
static const profile profiles[] = {{"plain", "Plain Wine", "wow64", "wine_bare", true},
                                   {"launcher", "Launcher", "wow64", "wine_bare", true},
                                   {"game_install", "Game Installer", "wow64", "wine_bare", true},
                                   {"m9", "D3D9 Metal", "wow64", "m9", true},
                                   {"m10", "D3D10 Metal", "wow64", "m10", true},
                                   {"m10_32", "D3D10 Metal (32-bit)", "win32", "m10_32", true},
                                   {"m11", "D3D11 Metal", "win64", "m11", true},
                                   {"m11_32", "D3D11 Metal (32-bit)", "win32", "m11_32", true},
                                   {"m12", "D3D12 Metal", "win64", "m12", true},
                                   {"m13", "GPTK D3DMetal", "win64", "m13", true},
                                   {"dotnet", ".NET", "win64", "wine_bare", true},
                                   {"win32_dotnet", "32-bit .NET", "win32", "m9", true},
                                   {"webview", "WebView", "wow64", "wine_bare", true},
                                   {"java_launcher", "Java Launcher", "wow64", "wine_bare", true},
                                   {"fna_arm64", "FNA / Mono ARM64", "win64", "fna_arm64", false},
                                   {"fna_x86", "FNA / Mono x86_64", "win64", "fna_arm64", false}};
char* ms_bottles_profiles_json(void) {
    ms_json_writer w;
    char* o;
    size_t i;
    ms_json_writer_init(&w);
    ms_json_writer_object_begin(&w);
    ms_json_writer_key(&w, "ok");
    ms_json_writer_bool(&w, true);
    ms_json_writer_key(&w, "profiles");
    ms_json_writer_array_begin(&w);
    for (i = 0; i < sizeof(profiles) / sizeof(profiles[0]); i++) {
        const profile* p = &profiles[i];
        ms_json_writer_object_begin(&w);
        obj_string(&w, "id", p->id);
        obj_string(&w, "name", p->name);
        obj_string(&w, "arch", p->arch);
        ms_json_writer_key(&w, "wineboot");
        ms_json_writer_bool(&w, p->wineboot);
        ms_json_writer_key(&w, "components");
        ms_json_writer_array_begin(&w);
        for (const char** component = profile_components(p->id); *component; component++)
            ms_json_writer_string(&w, *component);
        ms_json_writer_array_end(&w);
        if (!strcmp(p->id, "fna_arm64") || !strcmp(p->id, "fna_x86")) {
            bool arm64 = !strcmp(p->id, "fna_arm64");
            const char* home = getenv("METALSHARP_HOME");
            char* mono_base = path_join(home ? home : ".", arm64 ? "runtime/mono-arm64" : "runtime/mono-x86");
            char* mono_bin = mono_base ? path_join(mono_base, "bin/mono") : NULL;
            ms_json_writer_key(&w, "mono_runtime");
            ms_json_writer_object_begin(&w);
            obj_string(&w, "binary_path", mono_bin ? mono_bin : "");
            obj_string(&w, "config_path", arm64 ? "configs/terraria-mono.config" : "configs/celeste-x86-mono.config");
            obj_string(&w, "expected_arch", arm64 ? "arm64" : "x86_64");
            obj_string(&w, "id", arm64 ? "mono-arm64" : "mono-x86");
            obj_string(&w, "known_version", arm64 ? "6.14.1" : "6.12.0.122");
            obj_string(&w, "launch_wrapper", arm64 ? "native_mono_fna" : "arch -x86_64 native_mono_fna");
            obj_string(&w, "notes",
                       arm64 ? "Used by the Terraria-style FNA lane that worked through native macOS Mono plus dllmaps "
                               "and shims."
                             : "Used by the Celeste-style legacy lane where x86_64 Mono 6.12 and dllmaps avoid newer "
                               "ARM64-only assumptions.");
            ms_json_writer_object_end(&w);
            free(mono_base);
            free(mono_bin);
        } else {
            ms_json_writer_key(&w, "mono_runtime");
            ms_json_writer_null(&w);
        }
        obj_string(&w, "launch_pipeline", p->pipeline);
        ms_json_writer_object_end(&w);
    }
    ms_json_writer_array_end(&w);
    ms_json_writer_object_end(&w);
    o = ms_json_writer_take(&w);
    return o;
}
static char* saved_matrix_json(const char* home) {
    char *dir = path_join(home, "bottles"), *path = dir ? path_join(dir, "compatibility-matrix.json") : NULL, *raw;
    char e[64];
    ms_json* j;
    ms_json_writer w;
    char* o;
    if (!path) {
        free(dir);
        return NULL;
    }
    raw = read_text(path);
    free(dir);
    free(path);
    if (!raw)
        return NULL;
    j = ms_json_parse(raw, strlen(raw), e, sizeof(e));
    if (!j || ms_json_type_of(j) != MS_JSON_ARRAY) {
        ms_json_free(j);
        free(raw);
        return NULL;
    }
    ms_json_writer_init(&w);
    ms_json_writer_object_begin(&w);
    ms_json_writer_key(&w, "ok");
    ms_json_writer_bool(&w, true);
    ms_json_writer_key(&w, "cases");
    ms_json_writer_raw(&w, raw);
    ms_json_writer_object_end(&w);
    o = ms_json_writer_take(&w);
    ms_json_free(j);
    free(raw);
    return o;
}
char* ms_bottles_matrix_json(const char* home) {
    char* saved = saved_matrix_json(home);
    char er[96];
    ms_json* override_root = saved ? ms_json_parse(saved, strlen(saved), er, sizeof(er)) : NULL;
    const ms_json* override_cases = override_root ? ms_json_object_get(override_root, "cases") : NULL;
    static const char* id[] = {"minecraft-installer", "itch-windows-games",     "unity-demos",       "unreal-demos",
                               "electron-launchers",  "gog-offline-installers", "webview-launchers", "vc-redists"};
    static const char* name[] = {"Minecraft Installer",
                                 "Itch.io Windows Games",
                                 "Unity Demos",
                                 "Unreal Demos",
                                 "Electron Launchers",
                                 "GOG Offline Installers",
                                 "Epic/EA/Ubisoft Launchers",
                                 "VC Runtime Redistributables (2015-2022 + 2013)"};
    static const char* type[] = {"32-bit .NET/WinRT bootstrapper",
                                 "indie installer/extracted demo",
                                 "Unity player demo",
                                 "Unreal packaged demo",
                                 "Squirrel/Electron launcher",
                                 "offline game installer",
                                 "store-adjacent launcher",
                                 "runtime installer"};
    static const char* profile[] = {"win32_dotnet", "game_install", "m11",     "m12",
                                    "launcher",     "game_install", "webview", "plain"};
    static const char* opens[] = {"needs_real_trace", "untested", "untested", "untested",
                                  "untested",         "untested", "untested", "supported"};
    static const char* detected[] = {"pending", "pending", "pending", "pending",
                                     "pending", "pending", "pending", "not_applicable"};
    static const char* launches[] = {"pending", "pending", "pending", "pending",
                                     "pending", "pending", "pending", "not_applicable"};
    static const char* missing[] = {
        "Wine Mono/.NET 4.x compatibility",      "VC runtime or DirectX June 2010 varies by game",
        "VC runtime and Unity launcher handoff", "VC runtime, DirectX payloads, D3D12 route",
        "WebView/Gecko/browser runtime varies",  "VC runtime and DirectX June 2010",
        "WebView2 under Wine remains risky",     "local redistributable asset required"};
    ms_json_writer w;
    char* o;
    size_t i;
    ms_json_writer_init(&w);
    ms_json_writer_object_begin(&w);
    ms_json_writer_key(&w, "ok");
    ms_json_writer_bool(&w, true);
    ms_json_writer_key(&w, "cases");
    ms_json_writer_array_begin(&w);
    for (i = 0; i < 8; i++) {
        bool overridden = false;
        if (override_cases && ms_json_type_of(override_cases) == MS_JSON_ARRAY)
            for (size_t q = 0; q < ms_json_array_length(override_cases); q++) {
                char* override_id = NULL;
                ms_json_as_string(ms_json_object_get(ms_json_array_get(override_cases, q), "id"), &override_id);
                if (override_id && !strcmp(override_id, id[i]))
                    overridden = true;
                free(override_id);
            }
        if (overridden)
            continue;
        ms_json_writer_object_begin(&w);
        obj_string(&w, "id", id[i]);
        obj_string(&w, "name", name[i]);
        obj_string(&w, "case_type", type[i]);
        obj_string(&w, "required_profile", profile[i]);
        obj_string(&w, "installer_opens", opens[i]);
        obj_string(&w, "final_app_detected", detected[i]);
        obj_string(&w, "final_app_launches", launches[i]);
        obj_string(&w, "known_missing_runtime", missing[i]);
        ms_json_writer_key(&w, "bottle_id");
        ms_json_writer_null(&w);
        obj_string(&w, "notes", "");
        ms_json_writer_key(&w, "evidence_updated_at");
        ms_json_writer_null(&w);
        obj_string(&w, "per_game_prefix_recommendation", "not_applicable");
        ms_json_writer_object_end(&w);
    }
    if (override_cases && ms_json_type_of(override_cases) == MS_JSON_ARRAY)
        for (size_t k = 0; k < ms_json_array_length(override_cases); k++) {
            char* saved_case = ms_json_stringify(ms_json_array_get(override_cases, k));
            ms_json_writer_raw(&w, saved_case ? saved_case : "{}");
            free(saved_case);
        }
    ms_json_writer_array_end(&w);
    ms_json_writer_object_end(&w);
    o = ms_json_writer_take(&w);
    ms_json_free(override_root);
    free(saved);
    return o;
}
char* ms_bottles_redist_json(const char* home) {
    static const char* id[] = {"dotnet40",      "dotnet48",  "webview2",  "vcrun2019_x64",
                               "vcrun2019_x86", "vcrun2010", "vcrun2013", "directx_jun2010",
                               "openal",        "xna",       "physx"};
    static const char* name[] = {".NET Framework 4.0 Runtime",
                                 ".NET Framework 4.8 Runtime",
                                 "Microsoft Edge WebView2 Evergreen Runtime",
                                 "Microsoft Visual C++ 2015-2022 (x64)",
                                 "Microsoft Visual C++ 2015-2022 (x86)",
                                 "Microsoft Visual C++ 2010 Redistributable (10.0)",
                                 "Microsoft Visual C++ 2013 Redistributable (12.0)",
                                 "DirectX June 2010 Runtime",
                                 "OpenAL Runtime",
                                 "Microsoft XNA Framework 4.0",
                                 "NVIDIA PhysX Legacy Runtime"};
    static const char* url[] = {
        "https://dotnet.microsoft.com/en-us/download/dotnet-framework/net40",
        "https://dotnet.microsoft.com/en-us/download/dotnet-framework/net48",
        "https://developer.microsoft.com/en-us/microsoft-edge/webview2/",
        "https://aka.ms/vs/17/release/vc_redist.x64.exe",
        "https://aka.ms/vs/17/release/vc_redist.x86.exe",
        "https://www.microsoft.com/download/details.aspx?id=26999",
        ("https://support.microsoft.com/en-us/topic/"
         "update-for-visual-c-2013-redistributable-package-d8ccd6a4-4a90-bdbd-a060-8276036c0738"),
        "https://www.microsoft.com/download/details.aspx?id=8109",
        "https://www.openal.org/downloads/",
        "https://www.microsoft.com/download/details.aspx?id=20914",
        "https://www.nvidia.com/en-us/drivers/physx/physx-9-13-0604-legacy-driver/"};
    static const char* policy[] = {"official_download_or_user_supplied",
                                   "official_download_or_user_supplied",
                                   "official_download_or_user_supplied",
                                   "official_download",
                                   "official_download",
                                   "official_download_or_steam_commonredist",
                                   "official_download_or_steam_commonredist",
                                   "official_download_or_steam_commonredist",
                                   "official_download_or_steam_commonredist",
                                   "official_download_or_steam_commonredist",
                                   "official_download_or_steam_commonredist"};
    static const char* notes[] = {
        "Use for CLR v4 games that ship C++/CLI launchers, including older UE3 titles such as Goat Simulator.",
        "Use the official offline runtime installer; MetalSharp does not vendor it in this PR.",
        ("Use the Evergreen Standalone Installer for offline scenarios; Wine compatibility still needs per-installer "
         "evidence."),
        "Installs vcruntime140.dll, vcruntime140_1.dll, msvcp140.dll into system32",
        "Installs vcruntime140.dll, msvcp140.dll into syswow64",
        "Installs msvcr100.dll and msvcp100.dll. Required by older UE3/C++-CLI titles such as Goat Simulator.",
        "Installs msvcr120.dll and msvcp120.dll. Required by some older titles.",
        "Prefer Steam CommonRedist game payloads; local offline payload should contain DXSETUP.exe.",
        "Prefer Steam CommonRedist when available; older games often ship oalinst.exe beside installscript.vdf.",
        "Use local, Sharp Library staged, or Steam-provided XNA 4.0 redist assets; this stays receipt-driven per "
        "bottle.",
        "Only install when a game's install script or bundled redist explicitly requires legacy PhysX."};
    static const char* paths[][3] = {
        {"runtime/redist/DotNet/4.0/dotNetFx40_Client_x86_x64.exe",
         "runtime/redist/DotNet/4.0/dotNetFx40_Full_x86_x64.exe", NULL},
        {"runtime/redist/DotNet/4.8/ndp48-x86-x64-allos-enu.exe",
         "runtime/redist/DotNet/4.8/NDP48-x86-x64-AllOS-ENU.exe", NULL},
        {"runtime/redist/MicrosoftEdgeWebView2RuntimeInstallerX64.exe",
         "runtime/redist/MicrosoftEdgeWebView2RuntimeInstallerX86.exe", NULL},
        {"runtime/redist/vcredist/vc_redist.x64.exe", NULL, NULL},
        {"runtime/redist/vcredist/vc_redist.x86.exe", NULL, NULL},
        {"runtime/redist/vcredist/2010/vcredist_x64.exe", "runtime/redist/vcredist/2010/vcredist_x86.exe", NULL},
        {"runtime/redist/vcredist_x64.exe", "runtime/redist/vcredist_x86.exe", NULL},
        {"runtime/redist/DirectX/Jun2010/DXSETUP.exe", NULL, NULL},
        {"runtime/redist/OpenAL/oalinst.exe", "runtime/redist/oalinst.exe", NULL},
        {"runtime/redist/XNA/4.0/xnafx40_redist.msi", "runtime/redist/XNA/4.0/xnafx40_redist.exe",
         "bottles/*/installers/xnafx40_redist.msi"},
        {"runtime/redist/PhysX/PhysX-9.12.1031-SystemSoftware.msi",
         "runtime/redist/PhysX/PhysX-9.13.0604-SystemSoftware.msi", NULL}};
    ms_json_writer w;
    char* o;
    size_t i, k;
    ms_json_writer_init(&w);
    ms_json_writer_object_begin(&w);
    ms_json_writer_key(&w, "ok");
    ms_json_writer_bool(&w, true);
    ms_json_writer_key(&w, "sources");
    ms_json_writer_array_begin(&w);
    for (i = 0; i < sizeof(id) / sizeof(id[0]); i++) {
        ms_json_writer_object_begin(&w);
        obj_string(&w, "id", id[i]);
        obj_string(&w, "name", name[i]);
        obj_string(&w, "source_url", url[i]);
        ms_json_writer_key(&w, "local_targets");
        ms_json_writer_array_begin(&w);
        for (k = 0; k < 3; k++)
            if (paths[i][k]) {
                char* p;
                if (!strcmp(paths[i][k], "bottles/*/installers/xnafx40_redist.msi")) {
                    p = path_join(home, paths[i][k]);
                } else
                    p = path_join(home, paths[i][k]);
                ms_json_writer_string(&w, p ? p : paths[i][k]);
                free(p);
            }
        ms_json_writer_array_end(&w);
        obj_string(&w, "policy", policy[i]);
        obj_string(&w, "notes", notes[i]);
        ms_json_writer_object_end(&w);
    }
    ms_json_writer_array_end(&w);
    ms_json_writer_object_end(&w);
    o = ms_json_writer_take(&w);
    return o;
}
char* ms_bottles_contracts_json(void) {
    static const char* p[] = {"m9", "m10", "m11", "m12", "vkd3d", "m13", "fna_arm64", "wine_bare", "d3dmetal"};
    static const char* profile[] = {"m9", "m10", "m11", "m12", "vkd3d", "m13", "fna_arm64", "plain", "d3dmetal"};
    static const bool wine[] = {true, true, true, true, true, true, false, true, false};
    static const bool offline_route[] = {false, false, false, false, false, false, false, false, true};
    ms_json_writer w;
    char* o;
    size_t i;
    ms_json_writer_init(&w);
    ms_json_writer_object_begin(&w);
    ms_json_writer_key(&w, "ok");
    ms_json_writer_bool(&w, true);
    ms_json_writer_key(&w, "contracts");
    ms_json_writer_array_begin(&w);
    for (i = 0; i < 9; i++) {
        ms_json_writer_object_begin(&w);
        obj_string(&w, "pipeline", p[i]);
        obj_string(&w, "runtime_profile", profile[i]);
        obj_string(&w, "steam_identity_mode", offline_route[i] ? "offline_steam_emulation" : "wine_steam_background");
        obj_string(&w, "launch_route", offline_route[i] ? "/steam/launch-offline" : "/steam/launch-game");
        ms_json_writer_key(&w, "requires_wine");
        ms_json_writer_bool(&w, wine[i]);
        ms_json_writer_key(&w, "binds_to_shared_steam_prefix");
        ms_json_writer_bool(&w, wine[i]);
        ms_json_writer_key(&w, "waits_for_prefix_idle");
        ms_json_writer_bool(&w, false);
        obj_string(&w, "compat_tool_name", "MetalSharp");
        obj_string(&w, "bottle_id_template", "steam_{appid}");
        ms_json_writer_object_end(&w);
    }
    ms_json_writer_array_end(&w);
    ms_json_writer_object_end(&w);
    o = ms_json_writer_take(&w);
    return o;
}
