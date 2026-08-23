#include "metalsharp_backend/d3dmetal.h"
#include "metalsharp_backend/json.h"
#include "metalsharp_backend/json_writer.h"
#include "metalsharp_backend/steam.h"
#include "metalsharp_backend/steam_actions.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define GPTK_APP "/Applications/Game Porting Toolkit.app"
#define GPTK_ROOT GPTK_APP "/Contents/Resources/wine"
#define GPTK_WINE GPTK_ROOT "/bin/wine64"
#define GPTK_PE GPTK_ROOT "/lib/wine/x86_64-windows"
#define GPTK_FRAMEWORK GPTK_ROOT "/lib/external/D3DMetal.framework"
#define GPTK3_DMG_NAME "Game_Porting_Toolkit_3.0.dmg"
#define GPTK3_INNER_DMG_NAME "Evaluation environment for Windows games 3.0.dmg"
#define GPTK3_MSC_PKG_NAME "Metal Shader Converter 3.0.pkg"
#define GPTK3_MIN_DMG_SIZE (80ULL * 1024ULL * 1024ULL)

static const char* gptk_route_dlls[] = {"d3d10.dll", "d3d11.dll", "d3d12.dll", "dxgi.dll", "nvapi64.dll",
                                        "nvngx-on-metalfx.dll"};
static const char* gptk_vc_dlls[] = {"concrt140.dll",       "msvcp140.dll",       "msvcp140_1.dll",
                                     "msvcp140_2.dll",       "msvcp140_atomic_wait.dll", "msvcp140_codecvt_ids.dll",
                                     "vcomp140.dll",         "vcruntime140.dll",   "vcruntime140_1.dll"};
static const char* gptk3_route_dlls[] = {"d3d10.dll", "d3d11.dll", "d3d12.dll", "dxgi.dll", "nvapi64.dll",
                                         "nvngx-on-metalfx.dll", "atidxx64.dll"};
static const char* gptk3_route_unix[] = {"d3d10.so", "d3d11.so", "d3d12.so", "dxgi.so", "nvapi64.so",
                                         "nvngx-on-metalfx.so", "atidxx64.so"};
static char gptk3_overlay_error[PATH_MAX * 2];
typedef struct dstate {
    char id[129], name[256], game_dir[1024], game_exe[1024], error[256], last_launch_log[1024], last_launch_status[32],
        step[9][20];
    unsigned appid;
    unsigned last_launch_pid;
    bool ready;
    unsigned long long updated;
    struct dstate* next;
} dstate;
static dstate* states;
static void refresh_d3dmetal_state(const char* home, dstate* s);
static bool homebrew_gptk_ready(void);
static bool seed_gptk_prefix(const char* home);
static char* gptk_prefix(const char* home);
static bool framework_ready(const char* framework);
static void step(ms_json_writer* w, const char* key, const char* value);
static unsigned long long now_ms(void) {
    return (unsigned long long)time(NULL) * 1000ULL;
}
static ms_json* parse(const unsigned char* b, size_t n) {
    char e[64];
    return ms_json_parse(b ? (const char*)b : "", b ? n : 0, e, sizeof(e));
}
static bool num(const ms_json* j, const char* k, unsigned long long* o) {
    long long n;
    bool ok = ms_json_as_i64(ms_json_object_get(j, k), &n) && n > 0;
    if (ok)
        *o = (unsigned long long)n;
    return ok;
}
static char* text(const ms_json* j, const char* a, const char* b) {
    char* s = NULL;
    if (!ms_json_as_string(ms_json_object_get(j, a), &s) && b)
        ms_json_as_string(ms_json_object_get(j, b), &s);
    return s;
}
static char* bad(const char* s) {
    ms_json_writer w;
    char* o;
    ms_json_writer_init(&w);
    ms_json_writer_object_begin(&w);
    ms_json_writer_key(&w, "ok");
    ms_json_writer_bool(&w, false);
    ms_json_writer_key(&w, "error");
    ms_json_writer_string(&w, s);
    ms_json_writer_object_end(&w);
    o = ms_json_writer_take(&w);
    return o;
}
static dstate* find_state_memory(const char* id) {
    dstate* s;
    for (s = states; s; s = s->next)
        if (!strcmp(s->id, id))
            return s;
    return NULL;
}
static char* path_join(const char* a, const char* b) {
    size_t x = strlen(a), y = strlen(b);
    char* p = malloc(x + y + 2);
    if (!p)
        return NULL;
    snprintf(p, x + y + 2, "%s/%s", a, b);
    return p;
}

static char* find_game_exe(const char* root, unsigned depth) {
    DIR* dir;
    struct dirent* entry;
    if (!root || depth > 8 || !(dir = opendir(root)))
        return NULL;
    while ((entry = readdir(dir)) != NULL) {
        char* path;
        struct stat info;
        size_t length;
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
            continue;
        path = path_join(root, entry->d_name);
        if (!path || stat(path, &info) != 0) {
            free(path);
            continue;
        }
        length = strlen(entry->d_name);
        if (S_ISREG(info.st_mode) && length > 4 && !strcasecmp(entry->d_name + length - 4, ".exe")) {
            closedir(dir);
            return path;
        }
        if (S_ISDIR(info.st_mode)) {
            char* found = find_game_exe(path, depth + 1);
            free(path);
            if (found) {
                closedir(dir);
                return found;
            }
        } else
            free(path);
    }
    closedir(dir);
    return NULL;
}
static bool mkdirs(const char* path) {
    char* p = strdup(path);
    size_t i;
    if (!p)
        return false;
    for (i = 1; p[i]; i++)
        if (p[i] == '/') {
            p[i] = 0;
            mkdir(p, 0755);
            p[i] = '/';
        }
    if (mkdir(p, 0755) != 0 && errno != EEXIST) {
        free(p);
        return false;
    }
    free(p);
    return true;
}

static bool file_ready(const char* path) {
    struct stat st;
    return path && stat(path, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0;
}

static bool framework_ready(const char* framework) {
    char* binary = framework ? path_join(framework, "Versions/A/D3DMetal") : NULL;
    char* resources = framework ? path_join(framework, "Resources") : NULL;
    char* version_resources = framework ? path_join(framework, "Versions/A/Resources") : NULL;
    {
        bool binary_ready = file_ready(binary);
        bool resources_ready = resources && access(resources, R_OK) == 0;
        bool version_resources_ready = version_resources && access(version_resources, R_OK) == 0;
        free(binary);
        free(resources);
        free(version_resources);
        return binary_ready && (resources_ready || version_resources_ready);
    }
}

static bool copy_file_checked(const char* source, const char* target) {
    FILE *in = fopen(source, "rb"), *out;
    unsigned char buffer[16384];
    size_t n;
    bool ok = false;
    if (!in)
        return false;
    out = fopen(target, "wb");
    if (!out)
        goto done;
    while ((n = fread(buffer, 1, sizeof(buffer), in)) > 0)
        if (fwrite(buffer, 1, n, out) != n)
            goto close_out;
    ok = ferror(in) == 0;
close_out:
    fclose(out);
done:
    fclose(in);
    return ok;
}

static bool run_process(const char* executable, char* const argv[], const char* prefix, const char* dyld) {
    pid_t pid = fork();
    int wait_status = 0;
    if (pid < 0)
        return false;
    if (pid == 0) {
        if (prefix)
            setenv("WINEPREFIX", prefix, 1);
        if (dyld)
            setenv("DYLD_FALLBACK_LIBRARY_PATH", dyld, 1);
        setenv("WINEDEBUG", "-all", 1);
        execv(executable, argv);
        _exit(127);
    }
    while (waitpid(pid, &wait_status, 0) < 0 && errno == EINTR)
        ;
    return WIFEXITED(wait_status) && WEXITSTATUS(wait_status) == 0;
}

static bool run_dittoo(const char* source, const char* target) {
    char* const argv[] = {(char*)"/usr/bin/ditto", (char*)source, (char*)target, NULL};
    return run_process("/usr/bin/ditto", argv, NULL, NULL);
}

static bool replace_framework(const char* source, const char* target) {
    char temp[PATH_MAX];
    char backup[PATH_MAX];
    char* const temp_remove[] = {(char*)"/bin/rm", (char*)"rm", (char*)"-rf", temp, NULL};
    char* const backup_remove[] = {(char*)"/bin/rm", (char*)"rm", (char*)"-rf", backup, NULL};
    bool had_target = access(target, F_OK) == 0;
    bool moved_target = false;
    snprintf(temp, sizeof(temp), "%s.metalsharp-tmp", target);
    snprintf(backup, sizeof(backup), "%s.metalsharp-backup", target);
    (void)run_process("/bin/rm", temp_remove, NULL, NULL);
    (void)run_process("/bin/rm", backup_remove, NULL, NULL);
    if (!run_dittoo(source, temp) || !framework_ready(temp)) {
        (void)run_process("/bin/rm", temp_remove, NULL, NULL);
        return false;
    }
    if (had_target) {
        if (rename(target, backup) != 0) {
            (void)run_process("/bin/rm", temp_remove, NULL, NULL);
            return false;
        }
        moved_target = true;
    }
    if (rename(temp, target) != 0) {
        if (moved_target)
            (void)rename(backup, target);
        (void)run_process("/bin/rm", temp_remove, NULL, NULL);
        return false;
    }
    if (!framework_ready(target)) {
        (void)run_process("/bin/rm", backup_remove, NULL, NULL);
        if (moved_target)
            (void)rename(backup, target);
        else
            (void)run_process("/bin/rm", temp_remove, NULL, NULL);
        return false;
    }
    if (moved_target)
        (void)run_process("/bin/rm", backup_remove, NULL, NULL);
    return true;
}

static char* shell_quote(const char* value) {
    size_t length = 2;
    char* result;
    char* out;
    if (!value)
        return NULL;
    for (const char* p = value; *p; p++)
        length += *p == '\'' ? 4 : 1;
    result = malloc(length + 1);
    if (!result)
        return NULL;
    out = result;
    *out++ = '\'';
    for (const char* p = value; *p; p++) {
        if (*p == '\'') {
            memcpy(out, "'\\''", 4);
            out += 4;
        } else
            *out++ = *p;
    }
    *out++ = '\'';
    *out = 0;
    return result;
}

static char* capture_command(const char* command, int* exit_code) {
    FILE* pipe;
    char buffer[4096];
    char* output = NULL;
    size_t length = 0;
    int status = -1;
    if (!command || !(pipe = popen(command, "r")))
        return NULL;
    while (fgets(buffer, sizeof(buffer), pipe)) {
        size_t count = strlen(buffer);
        char* next = realloc(output, length + count + 1);
        if (!next) {
            free(output);
            pclose(pipe);
            return NULL;
        }
        output = next;
        memcpy(output + length, buffer, count);
        length += count;
        output[length] = 0;
    }
    status = pclose(pipe);
    if (exit_code)
        *exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : 1;
    return output ? output : strdup("");
}

static char* gptk3_download_path(void) {
    const char* user_home = getenv("HOME");
    char* downloads = user_home ? path_join(user_home, "Downloads") : NULL;
    char* candidate = downloads ? path_join(downloads, GPTK3_DMG_NAME) : NULL;
    struct stat info;
    bool ready = candidate && stat(candidate, &info) == 0 && S_ISREG(info.st_mode) &&
                 (unsigned long long)info.st_size >= GPTK3_MIN_DMG_SIZE;
    free(downloads);
    if (!ready) {
        free(candidate);
        return NULL;
    }
    return candidate;
}

static char* parse_hdiutil_mount(const char* output) {
    char* copy;
    char* line;
    char* line_save = NULL;
    if (!output || !(copy = strdup(output)))
        return NULL;
    for (line = strtok_r(copy, "\n", &line_save); line; line = strtok_r(NULL, "\n", &line_save)) {
        char* token_save = NULL;
        char* device = strtok_r(line, " \t", &token_save);
        char* kind = strtok_r(NULL, " \t", &token_save);
        char* mount = strtok_r(NULL, "\r\n", &token_save);
        struct stat info;
        while (mount && isspace((unsigned char)*mount))
            mount++;
        if (device && kind && mount && !strncmp(device, "/dev/", 5) && stat(mount, &info) == 0 && S_ISDIR(info.st_mode)) {
            char* result = strdup(mount);
            free(copy);
            return result;
        }
    }
    free(copy);
    return NULL;
}

static char* attach_dmg(const char* path) {
    char* quoted = shell_quote(path);
    char* command;
    char* output;
    int code = 1;
    if (!quoted)
        return NULL;
    command = malloc(strlen(quoted) + 80);
    if (command)
        snprintf(command, strlen(quoted) + 80, "/usr/bin/hdiutil attach -nobrowse -readonly %s 2>&1", quoted);
    free(quoted);
    output = command ? capture_command(command, &code) : NULL;
    free(command);
    if (!output || code != 0) {
        free(output);
        return NULL;
    }
    {
        char* mount = parse_hdiutil_mount(output);
        free(output);
        return mount;
    }
}

static void detach_dmg(const char* mount) {
    char* const argv[] = {(char*)"/usr/bin/hdiutil", (char*)"detach", (char*)"-force", (char*)mount, NULL};
    if (mount)
        (void)run_process("/usr/bin/hdiutil", argv, NULL, NULL);
}

static bool install_msc_pkg(const char* pkg) {
    char* installer_args[] = {(char*)"/usr/sbin/installer", (char*)"-pkg", (char*)pkg, (char*)"-target", (char*)"/", NULL};
    bool installed = run_process("/usr/sbin/installer", installer_args, NULL, NULL);
    if (!installed) {
        char* quoted = shell_quote(pkg);
        char* command;
        char* escaped;
        char* script;
        char* osascript_args[4];
        size_t escaped_length = 0;
        if (!quoted)
            return false;
        command = malloc(strlen(quoted) + 32);
        if (command)
            snprintf(command, strlen(quoted) + 32, "installer -pkg %s -target /", quoted);
        free(quoted);
        if (!command)
            return false;
        for (const char* p = command; *p; p++)
            escaped_length += (*p == '\\' || *p == '"') ? 2 : 1;
        escaped = malloc(escaped_length + 1);
        if (!escaped) {
            free(command);
            return false;
        }
        {
            char* out = escaped;
            for (const char* p = command; *p; p++) {
                if (*p == '\\' || *p == '"')
                    *out++ = '\\';
                *out++ = *p;
            }
            *out = 0;
        }
        free(command);
        script = malloc(strlen(escaped) + 64);
        if (!script) {
            free(escaped);
            return false;
        }
        snprintf(script, strlen(escaped) + 64, "do shell script \"%s\" with administrator privileges", escaped);
        osascript_args[0] = (char*)"/usr/bin/osascript";
        osascript_args[1] = (char*)"-e";
        osascript_args[2] = script;
        osascript_args[3] = NULL;
        installed = run_process("/usr/bin/osascript", osascript_args, NULL, NULL);
        free(script);
        free(escaped);
    }
    if (!installed)
        return false;
    {
        const char* tools[] = {"/usr/local/bin/metal-shaderconverter", "/opt/homebrew/bin/metal-shaderconverter",
                               "/opt/metal-shaderconverter/bin/metal-shaderconverter"};
        for (size_t i = 0; i < sizeof(tools) / sizeof(tools[0]); i++)
            if (access(tools[i], X_OK) == 0) {
                char* const args[] = {(char*)tools[i], (char*)"--version", NULL};
                return run_process(tools[i], args, NULL, NULL);
            }
    }
    return false;
}

static bool overlay_gptk3_redist(const char* redist) {
    char source[PATH_MAX], target[PATH_MAX];
    char* framework_source = NULL;
    char* framework_target = NULL;
    bool ok = true;
    gptk3_overlay_error[0] = 0;
    for (size_t i = 0; i < sizeof(gptk3_route_dlls) / sizeof(gptk3_route_dlls[0]); i++) {
        snprintf(source, sizeof(source), "%s/lib/wine/x86_64-windows/%s", redist, gptk3_route_dlls[i]);
        snprintf(target, sizeof(target), "%s/%s", GPTK_PE, gptk3_route_dlls[i]);
        if (!file_ready(source)) {
            snprintf(gptk3_overlay_error, sizeof(gptk3_overlay_error), "GPTK 3 Windows payload missing: %s", source);
            return false;
        }
        if (!mkdirs(GPTK_PE) || !copy_file_checked(source, target)) {
            snprintf(gptk3_overlay_error, sizeof(gptk3_overlay_error), "could not copy GPTK 3 Windows payload: %s", target);
            return false;
        }
    }
    for (size_t i = 0; i < sizeof(gptk3_route_unix) / sizeof(gptk3_route_unix[0]); i++) {
        snprintf(source, sizeof(source), "%s/lib/wine/x86_64-unix/%s", redist, gptk3_route_unix[i]);
        snprintf(target, sizeof(target), "%s/lib/wine/x86_64-unix/%s", GPTK_ROOT, gptk3_route_unix[i]);
        if (!file_ready(source)) {
            snprintf(gptk3_overlay_error, sizeof(gptk3_overlay_error), "GPTK 3 Unix payload missing: %s", source);
            return false;
        }
        if (!mkdirs(GPTK_ROOT "/lib/wine/x86_64-unix") || !copy_file_checked(source, target)) {
            snprintf(gptk3_overlay_error, sizeof(gptk3_overlay_error), "could not copy GPTK 3 Unix payload: %s", target);
            return false;
        }
    }
    framework_source = path_join(redist, "lib/external/D3DMetal.framework");
    framework_target = strdup(GPTK_FRAMEWORK);
    if (ok && framework_source && framework_target) {
        ok = framework_ready(framework_source) && replace_framework(framework_source, framework_target);
        if (!ok)
            snprintf(gptk3_overlay_error, sizeof(gptk3_overlay_error), "could not replace GPTK 3 framework: %s", framework_target);
    } else
        ok = false;
    {
        char* source_file = path_join(redist, "lib/external/libd3dshared.dylib");
        char* target_file = path_join(GPTK_ROOT, "lib/external/libd3dshared.dylib");
        if (ok && source_file && target_file) {
            if (!file_ready(source_file) || !mkdirs(GPTK_ROOT "/lib/external") || !copy_file_checked(source_file, target_file)) {
                snprintf(gptk3_overlay_error, sizeof(gptk3_overlay_error), "could not copy GPTK 3 external payload: %s",
                         target_file);
                ok = false;
            }
        } else if (ok) {
            snprintf(gptk3_overlay_error, sizeof(gptk3_overlay_error), "GPTK 3 external payload paths could not be built");
            ok = false;
        }
        free(source_file);
        free(target_file);
    }
    free(framework_source);
    free(framework_target);
    return ok;
}

static bool stage_gptk3_route_into_prefix(const char* home) {
    char* prefix = gptk_prefix(home);
    char* system32 = prefix ? path_join(prefix, "drive_c/windows/system32") : NULL;
    bool ok = prefix && system32 && mkdirs(system32);
    for (size_t i = 0; ok && i < sizeof(gptk_route_dlls) / sizeof(gptk_route_dlls[0]); i++) {
        char source[PATH_MAX];
        char* target;
        snprintf(source, sizeof(source), "%s/%s", GPTK_PE, gptk_route_dlls[i]);
        target = path_join(system32, gptk_route_dlls[i]);
        ok = file_ready(source) && target && copy_file_checked(source, target);
        free(target);
    }
    free(prefix);
    free(system32);
    return ok;
}

static char* repair_gptk3_overlay(const char* home) {
    char* dmg = gptk3_download_path();
    char* outer = NULL;
    char* inner = NULL;
    char* error = NULL;
    char* pkg;
    char* inner_dmg;
    char* redist;
    if (!dmg)
        return strdup("Download Game Porting Toolkit 3.0 into ~/Downloads, then run Repair again");
    outer = attach_dmg(dmg);
    if (!outer) {
        error = strdup("Could not mount the GPTK 3.0 DMG");
        goto done;
    }
    pkg = path_join(outer, GPTK3_MSC_PKG_NAME);
    inner_dmg = path_join(outer, GPTK3_INNER_DMG_NAME);
    if (!pkg || !inner_dmg || access(pkg, R_OK) != 0) {
        error = strdup("Metal Shader Converter 3.0.pkg was not found in the GPTK 3.0 DMG");
        free(pkg);
        free(inner_dmg);
        goto detach_outer;
    }
    if (!install_msc_pkg(pkg)) {
        error = strdup("Metal Shader Converter installation failed");
        free(pkg);
        free(inner_dmg);
        goto detach_outer;
    }
    inner = attach_dmg(inner_dmg);
    free(pkg);
    free(inner_dmg);
    if (!inner) {
        error = strdup("Could not mount the GPTK 3.0 evaluation environment DMG");
        goto detach_outer;
    }
    redist = path_join(inner, "redist");
    if (!redist || access(redist, R_OK) != 0 || !overlay_gptk3_redist(redist))
        error = strdup(gptk3_overlay_error[0] ? gptk3_overlay_error : "GPTK 3.0 redist overlay is incomplete");
    free(redist);
    detach_dmg(inner);
detach_outer:
    detach_dmg(outer);
done:
    free(dmg);
    free(outer);
    free(inner);
    if (!error && (!homebrew_gptk_ready() || !stage_gptk3_route_into_prefix(home)))
        error = strdup("GPTK 3.0 was overlaid, but the GPTK prefix could not be reseeded");
    if (!error) {
        char* marker = path_join(home, ".gptk3-installed");
        FILE* file = marker ? fopen(marker, "wb") : NULL;
        if (!file)
            error = strdup("Could not write the GPTK 3.0 installation marker");
        else {
            fputs(GPTK3_DMG_NAME "\n", file);
            fclose(file);
        }
        free(marker);
    }
    return error;
}

static bool homebrew_gptk_ready(void) {
    char path[PATH_MAX];
    if (access(GPTK_WINE, X_OK) != 0 || access(GPTK_FRAMEWORK, R_OK) != 0)
        return false;
    for (size_t i = 0; i < sizeof(gptk_route_dlls) / sizeof(gptk_route_dlls[0]); i++) {
        snprintf(path, sizeof(path), "%s/%s", GPTK_PE, gptk_route_dlls[i]);
        if (!file_ready(path))
            return false;
    }
    return true;
}

static bool rosetta_ready(void) {
    return access("/Library/Apple/System/Library/LaunchDaemons/com.apple.oahd.plist", F_OK) == 0 ||
           access("/usr/libexec/rosetta/oahd", X_OK) == 0;
}

static char* gptk_prefix(const char* home) {
    return path_join(home, "prefix-gptk");
}

static bool gptk_prefix_route_ready(const char* home) {
    char* prefix = gptk_prefix(home);
    char* system32 = prefix ? path_join(prefix, "drive_c/windows/system32") : NULL;
    char* marker = prefix ? path_join(prefix, ".gptk-ready") : NULL;
    char* steam = prefix ? path_join(prefix, "drive_c/Program Files (x86)/Steam/Steam.exe") : NULL;
    char* dosdevices = prefix ? path_join(prefix, "dosdevices") : NULL;
    char path[PATH_MAX];
    bool ok = marker && system32 && steam && dosdevices && access(marker, F_OK) == 0 && access(steam, F_OK) == 0 &&
              access(dosdevices, F_OK) == 0;
    for (size_t i = 0; ok && i < sizeof(gptk_route_dlls) / sizeof(gptk_route_dlls[0]); i++) {
        snprintf(path, sizeof(path), "%s/%s", system32, gptk_route_dlls[i]);
        ok = file_ready(path);
    }
    free(prefix);
    free(system32);
    free(marker);
    free(steam);
    free(dosdevices);
    return ok;
}

static bool gptk_vcpp_ready(const char* home) {
    char* prefix = gptk_prefix(home);
    char* system32 = prefix ? path_join(prefix, "drive_c/windows/system32") : NULL;
    char* syswow64 = prefix ? path_join(prefix, "drive_c/windows/syswow64") : NULL;
    const char* required64[] = {"vcruntime140.dll", "vcruntime140_1.dll", "msvcp140.dll"};
    const char* required32[] = {"vcruntime140.dll", "msvcp140.dll"};
    bool ok = prefix && system32 && syswow64;
    for (size_t i = 0; ok && i < sizeof(required64) / sizeof(required64[0]); i++) {
        char* p = path_join(system32, required64[i]);
        ok = file_ready(p);
        free(p);
    }
    for (size_t i = 0; ok && i < sizeof(required32) / sizeof(required32[0]); i++) {
        char* p = path_join(syswow64, required32[i]);
        ok = file_ready(p);
        free(p);
    }
    free(prefix);
    free(system32);
    free(syswow64);
    return ok;
}

static bool install_homebrew_gptk(void) {
    const char* brew = access("/opt/homebrew/bin/brew", X_OK) == 0 ? "/opt/homebrew/bin/brew" : "/usr/local/bin/brew";
    char* tap[] = {NULL, (char*)"tap", (char*)"gcenx/wine", NULL};
    char* trust[] = {NULL, (char*)"trust", (char*)"--cask", (char*)"gcenx/wine/game-porting-toolkit", NULL};
    char* install[] = {NULL, (char*)"install", (char*)"--cask", (char*)"gcenx/wine/game-porting-toolkit", NULL};
    if (access(brew, X_OK) != 0)
        return false;
    tap[0] = (char*)brew;
    trust[0] = (char*)brew;
    install[0] = (char*)brew;
    return run_process(brew, tap, NULL, NULL) && run_process(brew, trust, NULL, NULL) &&
           run_process(brew, install, NULL, NULL) && homebrew_gptk_ready();
}

static bool install_rosetta(void) {
    char* const argv[] = {(char*)"softwareupdate", (char*)"--install-rosetta", (char*)"--agree-to-license", NULL};
    return run_process("/usr/sbin/softwareupdate", argv, NULL, NULL) && rosetta_ready();
}

static bool seed_gptk_prefix(const char* home) {
    char* prefix = gptk_prefix(home);
    char* system32 = prefix ? path_join(prefix, "drive_c/windows/system32") : NULL;
    char* syswow64 = prefix ? path_join(prefix, "drive_c/windows/syswow64") : NULL;
    char* steam_source = path_join(home, "prefix-steam/drive_c/Program Files (x86)/Steam");
    char* steam_target = prefix ? path_join(prefix, "drive_c/Program Files (x86)/Steam") : NULL;
    char* users_source = path_join(home, "prefix-steam/drive_c/users");
    char* users_target = prefix ? path_join(prefix, "drive_c/users") : NULL;
    char* dyld = path_join(GPTK_ROOT, "lib");
    char dyld_value[PATH_MAX * 3];
    bool ok = prefix && system32 && syswow64 && mkdirs(system32) && mkdirs(syswow64);
    snprintf(dyld_value, sizeof(dyld_value), "%s:%s/lib/wine/x86_64-unix:%s/lib/external", GPTK_ROOT, GPTK_ROOT,
             GPTK_ROOT);
    if (ok) {
        char* const wineboot[] = {(char*)GPTK_WINE, (char*)"wineboot", (char*)"--init", NULL};
        ok = run_process(GPTK_WINE, wineboot, prefix, dyld_value);
    }
    for (size_t i = 0; ok && i < sizeof(gptk_route_dlls) / sizeof(gptk_route_dlls[0]); i++) {
        char source[PATH_MAX], target[PATH_MAX];
        snprintf(source, sizeof(source), "%s/%s", GPTK_PE, gptk_route_dlls[i]);
        snprintf(target, sizeof(target), "%s/%s", system32, gptk_route_dlls[i]);
        ok = file_ready(source) && copy_file_checked(source, target);
    }
    if (ok && !gptk_vcpp_ready(home)) {
        char x64_source[PATH_MAX], x86_source[PATH_MAX];
        snprintf(x64_source, sizeof(x64_source), "%s/runtime/wine/lib/wine/x86_64-windows", home);
        snprintf(x86_source, sizeof(x86_source), "%s/runtime/wine/lib/wine/i386-windows", home);
        for (size_t i = 0; ok && i < sizeof(gptk_vc_dlls) / sizeof(gptk_vc_dlls[0]); i++) {
            char source[PATH_MAX], target[PATH_MAX];
            snprintf(source, sizeof(source), "%s/%s", x64_source, gptk_vc_dlls[i]);
            snprintf(target, sizeof(target), "%s/%s", system32, gptk_vc_dlls[i]);
            if (file_ready(source))
                ok = copy_file_checked(source, target);
        }
        for (size_t i = 0; ok && i < sizeof(gptk_vc_dlls) / sizeof(gptk_vc_dlls[0]); i++) {
            char source[PATH_MAX], target[PATH_MAX];
            snprintf(source, sizeof(source), "%s/%s", x86_source, gptk_vc_dlls[i]);
            snprintf(target, sizeof(target), "%s/%s", syswow64, gptk_vc_dlls[i]);
            if (file_ready(source))
                ok = copy_file_checked(source, target);
        }
    }
    if (ok && steam_source && access(steam_source, F_OK) == 0)
        ok = run_dittoo(steam_source, steam_target);
    if (ok && users_source && access(users_source, F_OK) == 0)
        ok = run_dittoo(users_source, users_target);
    if (ok) {
        char* dosdevices = path_join(prefix, "dosdevices");
        char* c_link = dosdevices ? path_join(dosdevices, "c:") : NULL;
        mkdirs(dosdevices);
        if (c_link && access(c_link, F_OK) != 0)
            (void)symlink("../drive_c", c_link);
        free(dosdevices);
        free(c_link);
        char* marker = path_join(prefix, ".gptk-ready");
        if (marker) {
            FILE* f = fopen(marker, "wb");
            if (f) {
                fputs("ready\n", f);
                fclose(f);
            } else
                ok = false;
        }
        free(marker);
    }
    free(prefix);
    free(system32);
    free(syswow64);
    free(steam_source);
    free(steam_target);
    free(users_source);
    free(users_target);
    free(dyld);
    return ok && gptk_prefix_route_ready(home);
}
static char* read_file(const char* path) {
    FILE* f = fopen(path, "rb");
    long n;
    size_t got;
    char* s;
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
static void copy_field(char* dst, size_t n, const ms_json* j, const char* key, const char* fallback) {
    char* s = NULL;
    if (!ms_json_as_string(ms_json_object_get(j, key), &s))
        s = strdup(fallback);
    snprintf(dst, n, "%s", s ? s : "");
    free(s);
}
static dstate* load_state(const char* home, const char* id) {
    char *d = path_join(home, "d3dmetal-gptk/bottles"), *p, *raw;
    char e[64];
    ms_json* j;
    dstate* s;
    long long appid;
    bool ready;
    if (!d)
        return NULL;
    p = path_join(d, id);
    free(d);
    if (!p)
        return NULL;
    d = path_join(p, "state.json");
    free(p);
    raw = d ? read_file(d) : NULL;
    free(d);
    if (!raw)
        return NULL;
    j = ms_json_parse(raw, strlen(raw), e, sizeof(e));
    free(raw);
    if (!j || ms_json_type_of(j) != MS_JSON_OBJECT) {
        ms_json_free(j);
        return NULL;
    }
    s = calloc(1, sizeof(*s));
    if (!s) {
        ms_json_free(j);
        return NULL;
    }
    copy_field(s->id, sizeof(s->id), j, "bottle_id", id);
    copy_field(s->name, sizeof(s->name), j, "name", "D3DMetal Game");
    copy_field(s->game_dir, sizeof(s->game_dir), j, "game_dir", "");
    copy_field(s->game_exe, sizeof(s->game_exe), j, "game_exe", "");
    copy_field(s->error, sizeof(s->error), j, "last_error", "");
    copy_field(s->step[0], 20, j, "gptk_homebrew", "missing");
    copy_field(s->step[1], 20, j, "rosetta", "missing");
    copy_field(s->step[2], 20, j, "gptk_payload", "missing");
    copy_field(s->step[3], 20, j, "x64_redist", "missing");
    copy_field(s->step[4], 20, j, "seed", "missing");
    copy_field(s->step[5], 20, j, "gptk3", "missing");
    copy_field(s->last_launch_log, sizeof(s->last_launch_log), j, "last_launch_log", "");
    copy_field(s->last_launch_status, sizeof(s->last_launch_status), j, "last_launch_status", "");
    {
        long long launch_pid;
        if (ms_json_as_i64(ms_json_object_get(j, "last_launch_pid"), &launch_pid) && launch_pid > 0)
            s->last_launch_pid = (unsigned)launch_pid;
    }
    if (ms_json_as_i64(ms_json_object_get(j, "appid"), &appid) && appid > 0)
        s->appid = (unsigned)appid;
    if (ms_json_as_bool(ms_json_object_get(j, "play_ready"), &ready))
        s->ready = ready;
    if (ms_json_as_i64(ms_json_object_get(j, "updated_at"), &appid) && appid > 0)
        s->updated = (unsigned long long)appid;
    ms_json_free(j);
    s->next = states;
    states = s;
    return s;
}
static void step(ms_json_writer* w, const char* key, const char* value) {
    ms_json_writer_key(w, key);
    ms_json_writer_string(w, value);
}
static void state_json(ms_json_writer* w, const dstate* s) {
    ms_json_writer_object_begin(w);
    ms_json_writer_key(w, "schema");
    ms_json_writer_u64(w, 1);
    step(w, "bottle_id", s->id);
    ms_json_writer_key(w, "appid");
    ms_json_writer_u64(w, s->appid);
    step(w, "name", s->name);
    step(w, "game_dir", s->game_dir);
    ms_json_writer_key(w, "game_exe");
    if (s->game_exe[0])
        ms_json_writer_string(w, s->game_exe);
    else
        ms_json_writer_null(w);
    step(w, "gptk_homebrew", s->step[0]);
    step(w, "rosetta", s->step[1]);
    step(w, "gptk_payload", s->step[2]);
    step(w, "x64_redist", s->step[3]);
    step(w, "seed", s->step[4]);
    step(w, "gptk3", s->step[5]);
    ms_json_writer_key(w, "play_ready");
    ms_json_writer_bool(w, s->ready);
    ms_json_writer_key(w, "last_error");
    if (s->error[0])
        ms_json_writer_string(w, s->error);
    else
        ms_json_writer_null(w);
    ms_json_writer_key(w, "last_launch_pid");
    if (s->last_launch_pid)
        ms_json_writer_u64(w, s->last_launch_pid);
    else
        ms_json_writer_null(w);
    ms_json_writer_key(w, "last_launch_log");
    if (s->last_launch_log[0])
        ms_json_writer_string(w, s->last_launch_log);
    else
        ms_json_writer_null(w);
    ms_json_writer_key(w, "last_launch_status");
    if (s->last_launch_status[0])
        ms_json_writer_string(w, s->last_launch_status);
    else
        ms_json_writer_null(w);
    ms_json_writer_key(w, "updated_at");
    ms_json_writer_u64(w, s->updated);
    ms_json_writer_object_end(w);
}
static bool save_state(const char* home, const dstate* s) {
    char *d = path_join(home, "d3dmetal-gptk/bottles"), *p, *raw;
    FILE* f;
    ms_json_writer w;
    if (!d || !mkdirs(d)) {
        free(d);
        return false;
    }
    p = path_join(d, s->id);
    free(d);
    if (!p || !mkdirs(p)) {
        free(p);
        return false;
    }
    d = path_join(p, "state.json");
    free(p);
    if (!d)
        return false;
    ms_json_writer_init(&w);
    state_json(&w, s);
    raw = ms_json_writer_take(&w);
    f = fopen(d, "wb");
    if (!f || !raw || fputs(raw, f) < 0) {
        if (f)
            fclose(f);
        free(d);
        free(raw);
        return false;
    }
    fclose(f);
    free(d);
    free(raw);
    return true;
}

static bool persist_d3dmetal_manifest(const char* home, const dstate* s) {
    char* bottles = path_join(home, "bottles");
    char* directory = bottles ? path_join(bottles, s->id) : NULL;
    char* path = directory ? path_join(directory, "bottle.json") : NULL;
    char* raw = path ? read_file(path) : NULL;
    char error[64];
    ms_json* manifest = raw ? ms_json_parse(raw, strlen(raw), error, sizeof(error)) : NULL;
    ms_json_writer writer;
    char* serialized = NULL;
    FILE* file = NULL;
    bool ok = false;
    if (!manifest || ms_json_type_of(manifest) != MS_JSON_OBJECT)
        goto done;
    ms_json_writer_init(&writer);
    ms_json_writer_object_begin(&writer);
    for (size_t i = 0; i < ms_json_object_length(manifest); i++) {
        const char* key = ms_json_object_key_at(manifest, i);
        ms_json_writer_key(&writer, key);
        if (!strcmp(key, "runtime_profile"))
            ms_json_writer_string(&writer, "d3dmetal");
        else if (!strcmp(key, "preferred_pipeline"))
            ms_json_writer_string(&writer, "d3dmetal");
        else if (!strcmp(key, "arch"))
            ms_json_writer_string(&writer, "win64");
        else if (!strcmp(key, "game_install_path")) {
            if (s->game_dir[0])
                ms_json_writer_string(&writer, s->game_dir);
            else
                ms_json_writer_null(&writer);
        }
        else if (!strcmp(key, "health"))
            ms_json_writer_string(&writer, s->ready ? "ready" : "needs_repair");
        else if (!strcmp(key, "last_launch_pid")) {
            if (s->last_launch_pid)
                ms_json_writer_u64(&writer, s->last_launch_pid);
            else
                ms_json_writer_null(&writer);
        } else if (!strcmp(key, "last_launch_status")) {
            if (s->last_launch_status[0])
                ms_json_writer_string(&writer, s->last_launch_status);
            else
                ms_json_writer_null(&writer);
        } else {
            char* value = ms_json_stringify(ms_json_object_value_at(manifest, i));
            ms_json_writer_raw(&writer, value ? value : "null");
            free(value);
        }
    }
    if (s->game_dir[0] && !ms_json_object_get(manifest, "game_install_path")) {
        ms_json_writer_key(&writer, "game_install_path");
        ms_json_writer_string(&writer, s->game_dir);
    }
    ms_json_writer_object_end(&writer);
    serialized = ms_json_writer_take(&writer);
    file = path ? fopen(path, "wb") : NULL;
    if (file && serialized && fputs(serialized, file) >= 0)
        ok = true;
done:
    if (file)
        fclose(file);
    free(serialized);
    ms_json_free(manifest);
    free(raw);
    free(bottles);
    free(directory);
    free(path);
    return ok;
}
static dstate* state_for(const char* home, const char* id) {
    dstate* s = find_state_memory(id);
    if (s)
        return s;
    s = load_state(home, id);
    if (s)
        return s;
    {
        char* bottles = path_join(home, "bottles");
        char* directory = bottles ? path_join(bottles, id) : NULL;
        char* path = directory ? path_join(directory, "bottle.json") : NULL;
        char* raw = path ? read_file(path) : NULL;
        char error[64];
        ms_json* manifest = raw ? ms_json_parse(raw, strlen(raw), error, sizeof(error)) : NULL;
        char* profile = NULL;
        char* preferred = NULL;
        long long appid = 0;
        if (manifest && ms_json_type_of(manifest) == MS_JSON_OBJECT &&
            ms_json_as_i64(ms_json_object_get(manifest, "steam_app_id"), &appid) && appid > 0 &&
            ((ms_json_as_string(ms_json_object_get(manifest, "runtime_profile"), &profile) &&
              !strcmp(profile, "d3dmetal")) ||
             (ms_json_as_string(ms_json_object_get(manifest, "preferred_pipeline"), &preferred) &&
              !strcmp(preferred, "d3dmetal")))) {
            s = calloc(1, sizeof(*s));
            if (s) {
                snprintf(s->id, sizeof(s->id), "%s", id);
                s->appid = (unsigned)appid;
                copy_field(s->name, sizeof(s->name), manifest, "name", "D3DMetal Game");
                copy_field(s->game_dir, sizeof(s->game_dir), manifest, "game_install_path", "");
                if (!s->game_dir[0]) {
                    char* detected_dir = ms_steam_game_dir(home, (unsigned)appid);
                    if (detected_dir) {
                        snprintf(s->game_dir, sizeof(s->game_dir), "%s", detected_dir);
                        free(detected_dir);
                    }
                }
                {
                    char* detected = find_game_exe(s->game_dir, 0);
                    snprintf(s->game_exe, sizeof(s->game_exe), "%s", detected ? detected : "");
                    free(detected);
                }
                refresh_d3dmetal_state(home, s);
                s->next = states;
                states = s;
            }
        }
        free(profile);
        free(preferred);
        ms_json_free(manifest);
        free(raw);
        free(bottles);
        free(directory);
        free(path);
    }
    return s;
}

static dstate* state_from_steam_app(const char* home, unsigned long long appid, const char* id) {
    char* game_dir = ms_steam_game_dir(home, (unsigned)appid);
    dstate* s;
    if (!game_dir)
        return NULL;
    s = calloc(1, sizeof(*s));
    if (!s) {
        free(game_dir);
        return NULL;
    }
    snprintf(s->id, sizeof(s->id), "%s", id);
    s->appid = (unsigned)appid;
    snprintf(s->name, sizeof(s->name), "Game %llu", appid);
    snprintf(s->game_dir, sizeof(s->game_dir), "%s", game_dir);
    {
        char* detected = find_game_exe(game_dir, 0);
        snprintf(s->game_exe, sizeof(s->game_exe), "%s", detected ? detected : "");
        free(detected);
    }
    free(game_dir);
    refresh_d3dmetal_state(home, s);
    s->next = states;
    states = s;
    save_state(home, s);
    return s;
}

static bool d3dmetal_game_local_ready(const dstate* s) {
    char* slash;
    char* dir;
    bool ok = false;
    if (!s || !s->game_exe[0] || !(dir = strdup(s->game_exe)))
        return false;
    slash = strrchr(dir, '/');
    if (!slash)
        goto done;
    *slash = '\0';
    ok = true;
    for (size_t i = 0; ok && i < sizeof(gptk_route_dlls) / sizeof(gptk_route_dlls[0]); i++) {
        char* path = path_join(dir, gptk_route_dlls[i]);
        ok = file_ready(path);
        free(path);
    }
done:
    free(dir);
    return ok;
}

static bool stage_d3dmetal_game_local(const dstate* s) {
    char* dir;
    char* slash;
    bool ok = false;
    if (!s || !s->game_exe[0] || !(dir = strdup(s->game_exe)))
        return false;
    slash = strrchr(dir, '/');
    if (!slash)
        goto done;
    *slash = '\0';
    ok = true;
    for (size_t i = 0; ok && i < sizeof(gptk_route_dlls) / sizeof(gptk_route_dlls[0]); i++) {
        char source[PATH_MAX];
        char* target;
        snprintf(source, sizeof(source), "%s/%s", GPTK_PE, gptk_route_dlls[i]);
        target = path_join(dir, gptk_route_dlls[i]);
        ok = file_ready(source) && target && copy_file_checked(source, target);
        free(target);
    }
done:
    free(dir);
    return ok;
}

static bool gptk3_installed(const char* home) {
    char* marker = path_join(home, ".gptk3-installed");
    bool ready = marker && access(marker, F_OK) == 0;
    free(marker);
    return ready;
}

static void refresh_d3dmetal_state(const char* home, dstate* s) {
    bool gptk = homebrew_gptk_ready();
    bool prefix = gptk && rosetta_ready() && gptk_prefix_route_ready(home);
    snprintf(s->step[0], 20, "%s", gptk ? "installed" : "missing");
    snprintf(s->step[1], 20, "%s", rosetta_ready() ? "installed" : "missing");
    snprintf(s->step[2], 20, "%s", gptk ? "updated" : "missing");
    snprintf(s->step[3], 20, "%s", prefix && gptk_vcpp_ready(home) ? "installed" : "missing");
    snprintf(s->step[4], 20, "%s", prefix && gptk_vcpp_ready(home) && d3dmetal_game_local_ready(s) ? "seeded" : "missing");
    snprintf(s->step[5], 20, "%s", gptk3_installed(home) ? "installed" : "missing");
    s->ready = !strcmp(s->step[0], "installed") && !strcmp(s->step[1], "installed") &&
               !strcmp(s->step[2], "updated") && !strcmp(s->step[3], "installed") && !strcmp(s->step[4], "seeded");
}

static void actions_json(ms_json_writer* w, const dstate* s) {
    const char* ids[] = {"install_homebrew_gptk", "install_rosetta", "repair_gptk_payload",
                         "install_x64_redist",    "seed_prefix",     "play_d3dmetal"};
    const char* labels[] = {"Repair Homebrew GPTK", "Repair Rosetta", "Repair GPTK Payload",
                            "Repair Redist",        "Seed Prefix",    "Play D3DMetal"};
    const char* steps[] = {"gptk_homebrew", "rosetta", "gptk_payload", "x64_redist", "seed", "play_ready"};
    ms_json_writer_array_begin(w);
    for (size_t i = 0; i < 6; i++) {
        const char* sv = i == 0   ? s->step[0]
                         : i == 1 ? s->step[1]
                         : i == 2 ? s->step[2]
                         : i == 3 ? s->step[3]
                         : i == 4 ? s->step[4]
                                  : (s->ready ? "installed" : "missing");
        ms_json_writer_object_begin(w);
        step(w, "id", ids[i]);
        step(w, "label", labels[i]);
        ms_json_writer_key(w, "enabled");
        ms_json_writer_bool(w, i == 5 ? s->ready
                                      : strcmp(sv, "installed") != 0 && strcmp(sv, "updated") != 0 &&
                                            strcmp(sv, "seeded") != 0);
        step(w, "state", sv);
        step(w, "detail", steps[i]);
        ms_json_writer_object_end(w);
    }
    ms_json_writer_array_end(w);
}
static bool resolve_id(const ms_json* j, char* id, size_t n, unsigned long long* appid) {
    char* s = NULL;
    const ms_json* v = ms_json_object_get(j, "bottleId");
    if (!v)
        v = ms_json_object_get(j, "bottle_id");
    if (v)
        ms_json_as_string(v, &s);
    if (s && s[0])
        snprintf(id, n, "%s", s);
    else if (num(j, "appid", appid))
        snprintf(id, n, "steam_%llu", *appid);
    else {
        free(s);
        return false;
    }
    if (!*appid) {
        const ms_json* a = ms_json_object_get(j, "appid");
        if (a)
            num(j, "appid", appid);
    }
    free(s);
    return id[0] && strlen(id) <= 128;
}

static bool ensure_d3dmetal_bottle_manifest(const char* home, const char* id, unsigned long long appid, const char* name,
                                            const char* game_dir) {
    char* bottles = path_join(home, "bottles");
    char* directory = bottles ? path_join(bottles, id) : NULL;
    char* path = directory ? path_join(directory, "bottle.json") : NULL;
    bool ok = false;
    FILE* file;
    ms_json_writer writer;
    char* serialized;
    if (!home || !id || strstr(id, "..") || strchr(id, '/') || !directory || !path)
        goto done;
    if (access(path, R_OK) == 0) {
        ok = true;
        goto done;
    }
    if (!mkdirs(directory))
        goto done;
    ms_json_writer_init(&writer);
    ms_json_writer_object_begin(&writer);
    step(&writer, "id", id);
    step(&writer, "name", name && name[0] ? name : "D3DMetal Game");
    ms_json_writer_key(&writer, "custom_name");
    ms_json_writer_null(&writer);
    step(&writer, "bottle_type", "steam");
    ms_json_writer_key(&writer, "steam_app_id");
    ms_json_writer_u64(&writer, appid);
    {
        char* prefix = path_join(home, "prefix-steam");
        step(&writer, "prefix_path", prefix ? prefix : "");
        free(prefix);
    }
    step(&writer, "arch", "win64");
    step(&writer, "runtime_profile", "d3dmetal");
    step(&writer, "preferred_pipeline", "d3dmetal");
    ms_json_writer_key(&writer, "installed_components");
    ms_json_writer_array_begin(&writer);
    ms_json_writer_array_end(&writer);
    ms_json_writer_key(&writer, "game_install_path");
    ms_json_writer_string(&writer, game_dir ? game_dir : "");
    ms_json_writer_key(&writer, "runtime_assets");
    ms_json_writer_array_begin(&writer);
    ms_json_writer_array_end(&writer);
    ms_json_writer_key(&writer, "installed_app_detections");
    ms_json_writer_array_begin(&writer);
    ms_json_writer_array_end(&writer);
    step(&writer, "health", "needs_repair");
    ms_json_writer_key(&writer, "last_launch_log");
    ms_json_writer_null(&writer);
    ms_json_writer_key(&writer, "last_launch_pid");
    ms_json_writer_null(&writer);
    ms_json_writer_key(&writer, "last_launch_status");
    ms_json_writer_null(&writer);
    ms_json_writer_object_end(&writer);
    serialized = ms_json_writer_take(&writer);
    file = serialized ? fopen(path, "wb") : NULL;
    if (file && fputs(serialized, file) >= 0)
        ok = true;
    if (file)
        fclose(file);
    free(serialized);
done:
    free(bottles);
    free(directory);
    free(path);
    return ok;
}

char* ms_d3dmetal_json(const char* home, const char* action, const unsigned char* body, size_t len, int* status) {
    if (!strcmp(action, "repair-gptk3")) {
        char* downloaded = gptk3_download_path();
        if (!downloaded) {
            pid_t pid = fork();
            int wait_status = 0;
            if (pid == 0) {
                execl("/usr/bin/open", "open", "https://developer.apple.com/download/all/?q=game%20porting%20toolkit",
                      (char*)NULL);
                _exit(127);
            }
            if (pid >= 0)
                while (waitpid(pid, &wait_status, 0) < 0 && errno == EINTR) {
                }
            if (pid >= 0 && WIFEXITED(wait_status) && WEXITSTATUS(wait_status) == 0)
                return strdup("{\"ok\":true,\"download_opened\":true,\"download_required\":true,\"download_url\":\"https://"
                              "developer.apple.com/download/all/?q=game%20porting%20toolkit\"}");
            return bad("Could not launch /usr/bin/open");
        }
        free(downloaded);
    }
    ms_json* j = parse(body, len);
    char id[129] = {0}, *s, *game;
    unsigned long long appid = 0;
    dstate* st;
    ms_json_writer w;
    char* o;
    if (status)
        *status = 200;
    if (!j || ms_json_type_of(j) != MS_JSON_OBJECT) {
        ms_json_free(j);
        return bad("invalid JSON body");
    }
    if (!strcmp(action, "save")) {
        if (!num(j, "appid", &appid)) {
            ms_json_free(j);
            return bad("appid required");
        }
        if (!resolve_id(j, id, sizeof(id), &appid)) {
            ms_json_free(j);
            return bad("appid or bottleId required");
        }
        game = text(j, "gameDir", "game_dir");
        if (!game || !game[0]) {
            free(game);
            game = ms_steam_game_dir(home, (unsigned)appid);
        }
        if (!game || !game[0]) {
            free(game);
            ms_json_free(j);
            return bad("D3DMetal save requires a detected game install path");
        }
        s = text(j, "name", NULL);
        if (!ensure_d3dmetal_bottle_manifest(home, id, appid, s, game)) {
            free(s);
            free(game);
            ms_json_free(j);
            return bad("D3DMetal bottle manifest could not be created");
        }
        free(s);
        st = state_for(home, id);
        if (!st) {
            st = calloc(1, sizeof(*st));
            st->next = states;
            states = st;
        }
        snprintf(st->id, sizeof(st->id), "%s", id);
        st->appid = (unsigned)appid;
        s = text(j, "name", NULL);
        snprintf(st->name, sizeof(st->name), "%s", s && s[0] ? s : "D3DMetal Game");
        free(s);
        snprintf(st->game_dir, sizeof(st->game_dir), "%s", game);
        {
            char* detected = find_game_exe(game, 0);
            snprintf(st->game_exe, sizeof(st->game_exe), "%s", detected ? detected : "");
            free(detected);
        }
        refresh_d3dmetal_state(home, st);
        st->updated = now_ms();
        save_state(home, st);
        persist_d3dmetal_manifest(home, st);
        free(game);
        ms_json_free(j);
        goto respond_state;
    }
    if (!resolve_id(j, id, sizeof(id), &appid)) {
        ms_json_free(j);
        return bad("appid or bottleId required");
    }
    st = state_for(home, id);
    if (!st && appid > 0) {
        char canonical_id[129];
        snprintf(canonical_id, sizeof(canonical_id), "steam_%llu", appid);
        st = state_for(home, canonical_id);
        if (!st)
            st = state_from_steam_app(home, appid, canonical_id);
        if (st)
            snprintf(id, sizeof(id), "%s", canonical_id);
    }
    if (!st) {
        ms_json_free(j);
        {
            char e[220];
            snprintf(e, sizeof(e), "D3DMetal GPTK state not found for %s", id);
            return bad(e);
        }
    }
    if (!strcmp(action, "status")) {
        refresh_d3dmetal_state(home, st);
        st->updated = now_ms();
        save_state(home, st);
        persist_d3dmetal_manifest(home, st);
        ms_json_free(j);
        goto respond_state;
    }
    if (!strcmp(action, "install-homebrew-gptk")) {
        if (!install_homebrew_gptk()) {
            ms_json_free(j);
            return bad("Homebrew GPTK installation or payload verification failed");
        }
        snprintf(st->step[0], 20, "installed");
    } else if (!strcmp(action, "install-rosetta")) {
        if (!rosetta_ready() && !install_rosetta()) {
            ms_json_free(j);
            return bad("Rosetta installation failed");
        }
        snprintf(st->step[1], 20, "installed");
    } else if (!strcmp(action, "repair-gptk-payload")) {
        if (strcmp(st->step[0], "installed")) {
            free(j);
            return bad("Homebrew GPTK must be installed before repairing the payload");
        }
        if (!homebrew_gptk_ready()) {
            ms_json_free(j);
            return bad("Homebrew GPTK payload is incomplete");
        }
        snprintf(st->step[2], 20, "updated");
    } else if (!strcmp(action, "install-x64-redist")) {
        if (!seed_gptk_prefix(home) || !gptk_vcpp_ready(home)) {
            ms_json_free(j);
            return bad("VC++ runtime seeding into the GPTK prefix failed");
        }
        snprintf(st->step[3], 20, "installed");
    } else if (!strcmp(action, "seed-prefix")) {
        if (!gptk_vcpp_ready(home) || !seed_gptk_prefix(home) || !stage_d3dmetal_game_local(st)) {
            ms_json_free(j);
            return bad("GPTK prefix seeding failed");
        }
        snprintf(st->step[4], 20, "seeded");
    } else if (!strcmp(action, "repair-gptk3")) {
        char* repair_error = repair_gptk3_overlay(home);
        if (repair_error) {
            ms_json_free(j);
            o = bad(repair_error);
            free(repair_error);
            return o;
        }
        snprintf(st->step[5], 20, "installed");
        snprintf(st->step[2], 20, "updated");
    } else if (!strcmp(action, "play")) {
        if (!st->ready) {
            free(j);
            return bad("D3DMetal bottle is not ready to play");
        }
        free(j);
        {
            int launch_status = 500;
            o = ms_steam_launch_d3dmetal_json(home, st->appid, st->id, st->game_exe, &launch_status);
            if (status)
                *status = launch_status;
            if (!o)
                return bad("D3DMetal launch failed");
            if (launch_status < 400) {
                char launch_error[64];
                ms_json* launch = ms_json_parse(o, strlen(o), launch_error, sizeof(launch_error));
                long long launch_pid = 0;
                if (launch && ms_json_as_i64(ms_json_object_get(launch, "pid"), &launch_pid) && launch_pid > 0) {
                    st->last_launch_pid = (unsigned)launch_pid;
                    snprintf(st->last_launch_status, sizeof(st->last_launch_status), "running");
                    st->updated = now_ms();
                    save_state(home, st);
                    persist_d3dmetal_manifest(home, st);
                }
                ms_json_free(launch);
            }
            ms_json_writer_init(&w);
            ms_json_writer_object_begin(&w);
            ms_json_writer_key(&w, "ok");
            ms_json_writer_bool(&w, launch_status < 400);
            ms_json_writer_key(&w, "launch");
            if (launch_status < 400)
                ms_json_writer_raw(&w, o);
            else
                ms_json_writer_null(&w);
            ms_json_writer_object_end(&w);
            free(o);
            return ms_json_writer_take(&w);
        }
    } else {
        free(j);
        return bad("unknown D3DMetal action");
    }
    st->ready = !strcmp(st->step[0], "installed") && !strcmp(st->step[1], "installed") &&
                !strcmp(st->step[2], "updated") && !strcmp(st->step[3], "installed") && !strcmp(st->step[4], "seeded");
    st->updated = now_ms();
    save_state(home, st);
    persist_d3dmetal_manifest(home, st);
    free(j);
respond_state:
    ms_json_writer_init(&w);
    ms_json_writer_object_begin(&w);
    ms_json_writer_key(&w, "ok");
    ms_json_writer_bool(&w, true);
    ms_json_writer_key(&w, "state");
    state_json(&w, st);
    ms_json_writer_key(&w, "actions");
    actions_json(&w, st);
    ms_json_writer_key(&w, "gptk3_installed");
    ms_json_writer_bool(&w, gptk3_installed(home));
    ms_json_writer_key(&w, "gptk3_dmg_found");
    {
        const char* user_home = getenv("HOME");
        char* downloads = user_home ? path_join(user_home, "Downloads/Game_Porting_Toolkit_3.0.dmg") : NULL;
        struct stat dmg;
        ms_json_writer_bool(&w, downloads && stat(downloads, &dmg) == 0 && dmg.st_size >= 80 * 1024 * 1024);
        free(downloads);
    }
    ms_json_writer_object_end(&w);
    o = ms_json_writer_take(&w);
    return o;
}
