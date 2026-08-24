#include "metalsharp_backend/setup.h"

#include <CommonCrypto/CommonDigest.h>

#include "metalsharp_backend/backend.h"
#include "metalsharp_backend/json.h"
#include "metalsharp_backend/json_writer.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define MS_DXMT_VERSION                MS_BACKEND_VERSION "-m12-isolated-surface-v1"
#define MS_DXMT_MANIFEST               "metalsharp-dxmt-runtime.json"
#define MS_MOLTENVK_LIBRARY_SHA256     "8249d81ebf2d46f82b16ca166c2e5cca5d76d91d0a412cd6d3db1aaa6e8430bf"
#define MS_MOLTENVK_LANE_ICD_SHA256    "578ff08cd0d8734619357541771a5abc9c3470ca300030219a971a9e9dbbe466"
#define MS_MOLTENVK_RUNTIME_ICD_SHA256 "0dcbf7707cc0a347d0ba2941e835e5e92709919370a1bb0fc252e8dc4d95d322"
#define MS_DXMT_SCHEMA                 "metalsharp.dxmt-runtime.v1"

static const char* const dxmt_pe[] = {
    "d3d10core.dll", "d3d11.dll", "d3d12.dll", "dxgi.dll", "dxgi_dxmt.dll", "winemetal.dll", "nvapi64.dll", "nvngx.dll",
};
static const char* const dxmt_unix[] = {"winemetal.so"};
static const char* const dxmt_m12_unix[] = {"winemetal.so", "libc++.1.dylib", "libc++abi.1.dylib", "libunwind.1.dylib"};

static char* join_path(const char* left, const char* right) {
    size_t a = strlen(left), b = strlen(right);
    bool slash = a > 0 && left[a - 1] != '/';
    char* out = (char*)malloc(a + b + (slash ? 2 : 1));
    if (out != NULL)
        (void)snprintf(out, a + b + (slash ? 2 : 1), "%s%s%s", left, slash ? "/" : "", right);
    return out;
}

static bool file_nonempty(const char* path) {
    struct stat st;
    return path != NULL && stat(path, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0;
}

static char* setup_read_text(const char* path) {
    FILE* file = fopen(path, "rb");
    long length;
    size_t got;
    char* data;
    if (!file || fseek(file, 0, SEEK_END) != 0) {
        if (file)
            fclose(file);
        return NULL;
    }
    length = ftell(file);
    if (length < 0 || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return NULL;
    }
    data = malloc((size_t)length + 1);
    if (!data) {
        fclose(file);
        return NULL;
    }
    got = fread(data, 1, (size_t)length, file);
    fclose(file);
    data[got] = 0;
    return data;
}

static bool sha256_matches(const char* path, const char* expected) {
    FILE* file = fopen(path, "rb");
    CC_SHA256_CTX context;
    unsigned char buffer[8192], digest[CC_SHA256_DIGEST_LENGTH];
    size_t count;
    char actual[CC_SHA256_DIGEST_LENGTH * 2 + 1];
    if (!file || CC_SHA256_Init(&context) != 1) {
        if (file)
            fclose(file);
        return false;
    }
    while ((count = fread(buffer, 1, sizeof(buffer), file)) != 0)
        CC_SHA256_Update(&context, buffer, (CC_LONG)count);
    if (ferror(file) || CC_SHA256_Final(digest, &context) != 1) {
        fclose(file);
        return false;
    }
    fclose(file);
    for (size_t i = 0; i < CC_SHA256_DIGEST_LENGTH; i++)
        snprintf(actual + (i * 2), 3, "%02x", digest[i]);
    actual[sizeof(actual) - 1] = 0;
    return expected && strcmp(actual, expected) == 0;
}

static bool mkdir_p(const char* path);
static bool run_brew_install(const char* package);
static void optional_string(ms_json_writer*, const char*, const char*);

static char* find_bundle_archive(const char* home, const char* name) {
    const char* bundle_dir = getenv("METALSHARP_BUNDLE_DIR");
    const char* fixed[] = {
        "/Applications/MetalSharp.app/Contents/Resources/bundles",
        "/Applications/MetalSharp.app/Contents/Resources",
        "app/bundles",
    };
    char* candidate;
    size_t i;
    if (bundle_dir) {
        candidate = join_path(bundle_dir, name);
        if (file_nonempty(candidate))
            return candidate;
        free(candidate);
    }
    candidate = join_path(home, name);
    if (file_nonempty(candidate))
        return candidate;
    free(candidate);
    candidate = join_path(home, "bundles");
    if (candidate) {
        char* path = join_path(candidate, name);
        free(candidate);
        if (file_nonempty(path))
            return path;
        free(path);
    }
    candidate = join_path(home, "cache/bundles");
    if (candidate) {
        char* path = join_path(candidate, name);
        free(candidate);
        if (file_nonempty(path))
            return path;
        free(path);
    }
    for (i = 0; i < sizeof(fixed) / sizeof(fixed[0]); i++) {
        candidate = join_path(fixed[i], name);
        if (file_nonempty(candidate))
            return candidate;
        free(candidate);
    }
    return NULL;
}

static const char* fixed_zstd_path(void) {
    if (access("/opt/homebrew/bin/zstd", X_OK) == 0)
        return "/opt/homebrew/bin/zstd";
    if (access("/usr/local/bin/zstd", X_OK) == 0)
        return "/usr/local/bin/zstd";
    return NULL;
}

static bool child_succeeded(pid_t pid, int* wait_status) {
    while (waitpid(pid, wait_status, 0) < 0 && errno == EINTR) {
    }
    return WIFEXITED(*wait_status) && WEXITSTATUS(*wait_status) == 0;
}

static bool bundle_archive_paths_safe(const char* archive) {
    int compressed[2], listed[2];
    pid_t zstd_pid, tar_pid;
    int zstd_status = 0, tar_status = 0;
    FILE* output;
    char line[PATH_MAX * 2];
    bool safe = true;
    const char* zstd = fixed_zstd_path();
    if (!zstd || pipe(compressed) != 0 || pipe(listed) != 0)
        return false;
    zstd_pid = fork();
    if (zstd_pid == 0) {
        dup2(compressed[1], STDOUT_FILENO);
        close(compressed[0]);
        close(compressed[1]);
        close(listed[0]);
        close(listed[1]);
        execl(zstd, "zstd", "-d", "-c", archive, (char*)NULL);
        _exit(127);
    }
    if (zstd_pid < 0) {
        close(compressed[0]);
        close(compressed[1]);
        close(listed[0]);
        close(listed[1]);
        return false;
    }
    tar_pid = fork();
    if (tar_pid == 0) {
        dup2(compressed[0], STDIN_FILENO);
        dup2(listed[1], STDOUT_FILENO);
        close(compressed[0]);
        close(compressed[1]);
        close(listed[0]);
        close(listed[1]);
        execl("/usr/bin/tar", "tar", "-tf", "-", (char*)NULL);
        _exit(127);
    }
    close(compressed[0]);
    close(compressed[1]);
    close(listed[1]);
    if (tar_pid < 0) {
        close(listed[0]);
        kill(zstd_pid, SIGTERM);
        (void)child_succeeded(zstd_pid, &zstd_status);
        return false;
    }
    output = fdopen(listed[0], "r");
    if (!output) {
        close(listed[0]);
        kill(zstd_pid, SIGTERM);
        kill(tar_pid, SIGTERM);
        (void)child_succeeded(zstd_pid, &zstd_status);
        (void)child_succeeded(tar_pid, &tar_status);
        return false;
    }
    while (safe && fgets(line, sizeof(line), output)) {
        size_t length = strlen(line);
        while (length > 0 && (line[length - 1] == '\n' || line[length - 1] == '\r'))
            line[--length] = '\0';
        if (line[0] == '/' || !strcmp(line, "..") || strstr(line, "../") || strstr(line, "/..") || strchr(line, '\\'))
            safe = false;
    }
    fclose(output);
    if (!safe)
        kill(tar_pid, SIGTERM);
    if (!child_succeeded(zstd_pid, &zstd_status) || !child_succeeded(tar_pid, &tar_status))
        return false;
    return safe;
}

static bool extract_archive_to(const char* destination, const char* archive) {
    int compressed[2];
    pid_t zstd_pid, tar_pid;
    int zstd_status = 0, tar_status = 0;
    const char* zstd = fixed_zstd_path();
    if (!zstd || !bundle_archive_paths_safe(archive) || !mkdir_p(destination) || pipe(compressed) != 0)
        return false;
    zstd_pid = fork();
    if (zstd_pid == 0) {
        dup2(compressed[1], STDOUT_FILENO);
        close(compressed[0]);
        close(compressed[1]);
        execl(zstd, "zstd", "-d", "-c", archive, (char*)NULL);
        _exit(127);
    }
    if (zstd_pid < 0) {
        close(compressed[0]);
        close(compressed[1]);
        return false;
    }
    tar_pid = fork();
    if (tar_pid == 0) {
        dup2(compressed[0], STDIN_FILENO);
        close(compressed[0]);
        close(compressed[1]);
        execl("/usr/bin/tar", "tar", "-xf", "-", "-C", destination, (char*)NULL);
        _exit(127);
    }
    close(compressed[0]);
    close(compressed[1]);
    if (tar_pid < 0) {
        kill(zstd_pid, SIGTERM);
        (void)child_succeeded(zstd_pid, &zstd_status);
        return false;
    }
    return child_succeeded(zstd_pid, &zstd_status) && child_succeeded(tar_pid, &tar_status);
}

static bool extract_bundle_archive(const char* home, const char* archive) {
    return extract_archive_to(home, archive);
}

/* Runtime archives downloaded through a quarantined app or browser can cause
 * macOS to propagate com.apple.quarantine onto Wine executables. Rust extracts
 * through a clean temporary tree before copying runtime files; clear the same
 * inherited attribute here so metalsharp-wine --version is executable too. */
static void clear_runtime_quarantine(const char* home) {
    char* runtime = join_path(home, "runtime");
    pid_t pid;
    int wait_status;
    if (!runtime)
        return;
    pid = fork();
    if (pid == 0) {
        execl("/usr/bin/xattr", "xattr", "-dr", "com.apple.quarantine", runtime, (char*)NULL);
        _exit(127);
    }
    if (pid > 0)
        while (waitpid(pid, &wait_status, 0) < 0 && errno == EINTR) {
        }
    free(runtime);
}

static bool download_bundle_archive(const char* home, const char* name) {
    char *cache = join_path(home, "cache/bundles"), *destination, *temporary;
    pid_t pid, waited;
    int wait_status;
    if (!cache || !mkdir_p(cache)) {
        free(cache);
        return false;
    }
    destination = join_path(cache, name);
    temporary = destination ? malloc(strlen(destination) + 16) : NULL;
    if (temporary)
        snprintf(temporary, strlen(destination) + 16, "%s.download", destination);
    if (!destination || !temporary) {
        free(cache);
        free(destination);
        free(temporary);
        return false;
    }
    if (file_nonempty(destination)) {
        free(cache);
        free(destination);
        free(temporary);
        return true;
    }
    unlink(temporary);
    pid = fork();
    if (pid == 0) {
        char url[512];
        snprintf(url, sizeof(url), "https://github.com/aaf2tbz/metalsharp/releases/download/bundles/%s", name);
        execl("/usr/bin/curl", "curl", "--fail", "--location", "--silent", "--show-error", "--retry", "2",
              "--connect-timeout", "30", "--max-time", "600", "-o", temporary, url, (char*)NULL);
        _exit(127);
    }
    do
        waited = waitpid(pid, &wait_status, 0);
    while (waited < 0 && errno == EINTR);
    bool ok = waited == pid && WIFEXITED(wait_status) && WEXITSTATUS(wait_status) == 0 && file_nonempty(temporary);
    if (ok) {
        unlink(destination);
        ok = rename(temporary, destination) == 0;
    } else
        unlink(temporary);
    free(cache);
    free(destination);
    free(temporary);
    return ok;
}

static bool copy_directory_contents(const char* source, const char* destination) {
    char* source_contents = join_path(source, ".");
    pid_t pid;
    int wait_status;
    if (!source_contents || !mkdir_p(destination)) {
        free(source_contents);
        return false;
    }
    pid = fork();
    if (pid < 0) {
        free(source_contents);
        return false;
    }
    if (pid == 0) {
        execl("/bin/cp", "cp", "-R", source_contents, destination, (char*)NULL);
        _exit(127);
    }
    free(source_contents);
    while (waitpid(pid, &wait_status, 0) < 0 && errno == EINTR) {
    }
    return WIFEXITED(wait_status) && WEXITSTATUS(wait_status) == 0;
}

static void remove_path_tree(const char* path);
static bool copy_file_path(const char* source, const char* destination);

static char* find_setup_source(const char* relative) {
    char cwd[PATH_MAX];
    if (getcwd(cwd, sizeof(cwd))) {
        char* current = strdup(cwd);
        for (int i = 0; current && i < 8; i++) {
            char* candidate = join_path(current, relative);
            if (candidate && access(candidate, R_OK) == 0) {
                free(current);
                return candidate;
            }
            free(candidate);
            char* slash = strrchr(current, '/');
            if (!slash || slash == current)
                break;
            *slash = 0;
        }
        free(current);
    }
    return access(relative, R_OK) == 0 ? strdup(relative) : NULL;
}

static bool compile_fna_shim(const char* source, const char* destination, bool objective_c,
                             bool undefined_dynamic_lookup) {
    pid_t pid;
    int wait_status;
    char* args[32];
    char install_name[PATH_MAX];
    size_t n = 0;
    const char* output_name = strrchr(destination, '/') ? strrchr(destination, '/') + 1 : destination;
    snprintf(install_name, sizeof(install_name), "@loader_path/%s", output_name);
    args[n++] = (char*)"clang";
    args[n++] = (char*)"-dynamiclib";
    args[n++] = (char*)"-O2";
    args[n++] = (char*)"-arch";
    args[n++] = (char*)"arm64";
    args[n++] = (char*)"-arch";
    args[n++] = (char*)"x86_64";
    if (objective_c)
        args[n++] = (char*)"-fobjc-arc";
    if (undefined_dynamic_lookup) {
        args[n++] = (char*)"-undefined";
        args[n++] = (char*)"dynamic_lookup";
    }
    args[n++] = (char*)"-o";
    args[n++] = (char*)destination;
    args[n++] = (char*)source;
    args[n++] = (char*)"-install_name";
    args[n++] = install_name;
    if (objective_c) {
        args[n++] = (char*)"-framework";
        args[n++] = (char*)"Cocoa";
        args[n++] = (char*)"-framework";
        args[n++] = (char*)"Carbon";
    }
    args[n] = NULL;
    pid = fork();
    if (pid < 0)
        return false;
    if (pid == 0) {
        execvp("clang", args);
        _exit(127);
    }
    while (waitpid(pid, &wait_status, 0) < 0 && errno == EINTR) {
    }
    return WIFEXITED(wait_status) && WEXITSTATUS(wait_status) == 0 && file_nonempty(destination);
}

static void precompile_fna_shims(const char* home) {
    struct shim_spec {
        const char* output;
        const char* source;
        bool objc;
        bool undefined;
    };
    const struct shim_spec specs[] = {
        {"libkernel32.dylib", "src/fna/shims/kernel32_shim.c", false, false},
        {"libuser32.dylib", "src/fna/shims/user32_shim.c", false, false},
        {"libCarbon.dylib", "src/fna/shims/carbon_hiview_shim.m", true, false},
        {"libmetalsharp_carbon_interpose.dylib", "src/fna/shims/carbon_interpose.c", false, false},
        {"libSystem.Native.dylib", "src/fna/shims/system_native_stub.c", false, false},
        {"libgdiplus.dylib", "src/fna/terraria/gdiplus_stub.c", false, false},
        {"libFAudio.0.dylib", "src/fna/terraria/faudio_stub.c", false, false},
        {"libCSteamworks.dylib", "src/fna/shims/csteamworks_shim.c", false, true},
        {"libfmod.dylib", "src/fna/shims/fmod_stub.c", false, true},
        {"libfmodstudio.dylib", "src/fna/shims/fmodstudio_stub.c", false, true}};
    char* dir = join_path(home, "runtime/shims");
    if (!dir || !mkdir_p(dir)) {
        free(dir);
        return;
    }
    for (size_t i = 0; i < sizeof(specs) / sizeof(specs[0]); i++) {
        char* source = find_setup_source(specs[i].source);
        char* destination = join_path(dir, specs[i].output);
        if (source && destination && !file_nonempty(destination))
            (void)compile_fna_shim(source, destination, specs[i].objc, specs[i].undefined);
        free(source);
        free(destination);
    }
    const char* bundled[] = {"xaudio2_9.dylib", "xinput1_4.dylib"};
    for (size_t i = 0; i < sizeof(bundled) / sizeof(bundled[0]); i++) {
        char* destination = join_path(dir, bundled[i]);
        char* source_dir = join_path(home, "scripts/tools/native");
        char* source = source_dir ? join_path(source_dir, bundled[i]) : NULL;
        if ((!source || !file_nonempty(source)) && source_dir) {
            free(source);
            source = join_path(home, "runtime/shims");
            char* candidate = source ? join_path(source, bundled[i]) : NULL;
            free(source);
            source = candidate;
        }
        if (source && destination && file_nonempty(source) && !file_nonempty(destination))
            (void)copy_file_path(source, destination);
        free(destination);
        free(source_dir);
        free(source);
    }
    const struct {
        const char* output;
        const char* aliases[3];
    } links[] = {{"xaudio2_9.dylib", {"xaudio2_8.dylib", "xaudio2_7.dylib", NULL}},
                 {"xinput1_4.dylib", {"xinput1_3.dylib", "xinput9_1_0.dylib", NULL}},
                 {"libFAudio.0.dylib", {"libFAudio.dylib", NULL, NULL}}};
    for (size_t i = 0; i < sizeof(links) / sizeof(links[0]); i++) {
        char* output = join_path(dir, links[i].output);
        if (!output || !file_nonempty(output)) {
            free(output);
            continue;
        }
        for (size_t j = 0; j < 3 && links[i].aliases[j]; j++) {
            char* alias = join_path(dir, links[i].aliases[j]);
            if (alias && access(alias, F_OK) != 0)
                (void)symlink(links[i].output, alias);
            free(alias);
        }
        free(output);
    }
    free(dir);
}

static bool copy_file_path(const char* source, const char* destination) {
    char* parent = strdup(destination);
    char* slash;
    pid_t pid;
    int wait_status;
    if (!parent)
        return false;
    slash = strrchr(parent, '/');
    if (slash) {
        *slash = 0;
        if (!mkdir_p(parent)) {
            free(parent);
            return false;
        }
    }
    free(parent);
    pid = fork();
    if (pid < 0)
        return false;
    if (pid == 0) {
        execl("/bin/cp", "cp", source, destination, (char*)NULL);
        _exit(127);
    }
    while (waitpid(pid, &wait_status, 0) < 0 && errno == EINTR) {
    }
    return WIFEXITED(wait_status) && WEXITSTATUS(wait_status) == 0;
}

static bool extract_split_bundle(const char* home, const char* archive_name, const char* const (*mapping)[2],
                                 size_t mapping_count) {
    char* archive = find_bundle_archive(home, archive_name);
    char temp[PATH_MAX];
    bool ok = false;
    if (!archive)
        return false;
    snprintf(temp, sizeof(temp), "%s/.%s-extract-%ld", home, archive_name, (long)getpid());
    remove_path_tree(temp);
    if (mkdir(temp, 0700) != 0 && errno != EEXIST) {
        free(archive);
        return false;
    }
    if (extract_archive_to(temp, archive)) {
        ok = true;
        for (size_t i = 0; i < mapping_count; i++) {
            char* source = join_path(temp, mapping[i][0]);
            char* destination = join_path(home, mapping[i][1]);
            struct stat info;
            if (source && stat(source, &info) == 0 && S_ISDIR(info.st_mode) && destination)
                copy_directory_contents(source, destination);
            free(source);
            free(destination);
        }
    }
    remove_path_tree(temp);
    free(archive);
    return ok;
}

static void remove_path_tree(const char* path) {
    pid_t pid = fork();
    int wait_status;
    if (pid < 0)
        return;
    if (pid == 0) {
        execl("/bin/rm", "rm", "-rf", path, (char*)NULL);
        _exit(127);
    }
    while (waitpid(pid, &wait_status, 0) < 0 && errno == EINTR) {
    }
}

static bool write_dxmt_manifest(const char* directory) {
    char* path = join_path(directory, MS_DXMT_MANIFEST);
    FILE* file;
    ms_json_writer writer;
    char* serialized;
    bool ok = false;
    if (!path || !mkdir_p(directory)) {
        free(path);
        return false;
    }
    ms_json_writer_init(&writer);
    ms_json_writer_object_begin(&writer);
    optional_string(&writer, "schema", MS_DXMT_SCHEMA);
    optional_string(&writer, "version", MS_DXMT_VERSION);
    optional_string(&writer, "source", "bundled:metalsharp-graphics-dll.tar.zst");
    ms_json_writer_object_end(&writer);
    serialized = ms_json_writer_take(&writer);
    file = serialized ? fopen(path, "wb") : NULL;
    if (file) {
        ok = fputs(serialized, file) >= 0 && fclose(file) == 0;
        if (!ok)
            fclose(file);
    }
    free(serialized);
    free(path);
    return ok;
}

static bool mkdir_p(const char* path) {
    char* copy = strdup(path);
    size_t i;
    if (copy == NULL)
        return false;
    for (i = 1; copy[i] != '\0'; ++i) {
        if (copy[i] == '/') {
            copy[i] = '\0';
            if (mkdir(copy, 0755) != 0 && errno != EEXIST) {
                free(copy);
                return false;
            }
            copy[i] = '/';
        }
    }
    if (mkdir(copy, 0755) != 0 && errno != EEXIST) {
        free(copy);
        return false;
    }
    free(copy);
    return true;
}

static char* read_file(const char* path, size_t* length_out) {
    FILE* file = fopen(path, "rb");
    long size;
    char* data;
    size_t length;
    if (file == NULL || fseek(file, 0, SEEK_END) != 0) {
        if (file != NULL)
            fclose(file);
        return NULL;
    }
    size = ftell(file);
    if (size < 0 || (unsigned long long)size > 4ULL * 1024ULL * 1024ULL || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return NULL;
    }
    data = (char*)malloc((size_t)size + 1);
    if (data == NULL) {
        fclose(file);
        return NULL;
    }
    length = fread(data, 1, (size_t)size, file);
    fclose(file);
    data[length] = '\0';
    if (length_out != NULL)
        *length_out = length;
    return data;
}

static ms_json* read_json(const char* path) {
    size_t length;
    char error[128];
    char* text = read_file(path, &length);
    ms_json* json;
    if (text == NULL)
        return NULL;
    json = ms_json_parse(text, length, error, sizeof(error));
    free(text);
    return json;
}

static bool get_bool(const ms_json* object, const char* key, bool fallback) {
    bool value;
    return ms_json_as_bool(ms_json_object_get(object, key), &value) ? value : fallback;
}

static unsigned long long get_u64(const ms_json* object, const char* key, unsigned long long fallback) {
    long long value;
    return ms_json_as_i64(ms_json_object_get(object, key), &value) && value >= 0 ? (unsigned long long)value : fallback;
}

static char* get_string(const ms_json* object, const char* key, const char* fallback) {
    char* value = NULL;
    if (!ms_json_as_string(ms_json_object_get(object, key), &value))
        return strdup(fallback);
    return value;
}

static char* manifest_version(const char* runtime_dir) {
    char* path = join_path(runtime_dir, MS_DXMT_MANIFEST);
    ms_json* manifest;
    char* version = NULL;
    if (path == NULL)
        return NULL;
    manifest = read_json(path);
    free(path);
    if (manifest == NULL || ms_json_type_of(manifest) != MS_JSON_OBJECT) {
        ms_json_free(manifest);
        return NULL;
    }
    {
        char* schema = get_string(manifest, "schema", "");
        if (schema != NULL && strcmp(schema, MS_DXMT_SCHEMA) == 0)
            version = get_string(manifest, "version", "");
        free(schema);
    }
    ms_json_free(manifest);
    if (version != NULL && version[0] == '\0') {
        free(version);
        version = NULL;
    }
    return version;
}

static bool required_ready(const char* runtime_dir, const char* const* unix_files, size_t unix_count) {
    char* unix_dir = join_path(runtime_dir, "x86_64-unix");
    char* pe_dir = join_path(runtime_dir, "x86_64-windows");
    size_t i;
    bool ready = unix_dir != NULL && pe_dir != NULL;
    if (ready) {
        for (i = 0; i < unix_count; ++i) {
            char* path = join_path(unix_dir, unix_files[i]);
            ready = path != NULL && file_nonempty(path);
            free(path);
            if (!ready)
                break;
        }
    }
    if (ready) {
        for (i = 0; i < sizeof(dxmt_pe) / sizeof(dxmt_pe[0]); ++i) {
            char* path = join_path(pe_dir, dxmt_pe[i]);
            ready = path != NULL && file_nonempty(path);
            free(path);
            if (!ready)
                break;
        }
    }
    free(unix_dir);
    free(pe_dir);
    return ready;
}

static bool host_runtime_lib_ready(const char* home) {
    char* wine = join_path(home, "runtime/wine/lib/metalsharp");
    char* x64 = wine == NULL ? NULL : join_path(wine, "x86_64-windows/metalsharp_ntdll_hook.dll");
    char* x86 = wine == NULL ? NULL : join_path(wine, "i386-windows/metalsharp_ntdll_hook.dll");
    bool ready = file_nonempty(x64) && file_nonempty(x86);
    free(wine);
    free(x64);
    free(x86);
    return ready;
}

static void optional_string(ms_json_writer* writer, const char* key, const char* value) {
    ms_json_writer_key(writer, key);
    if (value == NULL)
        ms_json_writer_null(writer);
    else
        ms_json_writer_string(writer, value);
}

static char* runtime_status_json(const char* home) {
    char* dxmt = join_path(home, "runtime/wine/lib/dxmt");
    char* m12 = join_path(home, "runtime/wine/lib/dxmt_m12");
    char* dxmt_manifest = dxmt == NULL ? NULL : join_path(dxmt, MS_DXMT_MANIFEST);
    char* m12_manifest = m12 == NULL ? NULL : join_path(m12, MS_DXMT_MANIFEST);
    char* version = manifest_version(dxmt);
    char* m12_version = manifest_version(m12);
    bool ready = required_ready(dxmt, dxmt_unix, sizeof(dxmt_unix) / sizeof(dxmt_unix[0]));
    bool m12_ready = required_ready(m12, dxmt_m12_unix, sizeof(dxmt_m12_unix) / sizeof(dxmt_m12_unix[0]));
    bool current = ready && version != NULL && strcmp(version, MS_DXMT_VERSION) == 0;
    bool m12_current = m12_ready && m12_version != NULL && strcmp(m12_version, MS_DXMT_VERSION) == 0;
    ms_json_writer writer;
    char* result;
    ms_json_writer_init(&writer);
    ms_json_writer_object_begin(&writer);
    ms_json_writer_key(&writer, "current");
    ms_json_writer_bool(&writer, current);
    ms_json_writer_key(&writer, "filesReady");
    ms_json_writer_bool(&writer, ready);
    ms_json_writer_key(&writer, "m12Current");
    ms_json_writer_bool(&writer, m12_current);
    ms_json_writer_key(&writer, "m12FilesReady");
    ms_json_writer_bool(&writer, m12_ready);
    optional_string(&writer, "installedVersion", version);
    optional_string(&writer, "m12InstalledVersion", m12_version);
    ms_json_writer_key(&writer, "requiredVersion");
    ms_json_writer_string(&writer, MS_DXMT_VERSION);
    optional_string(&writer, "manifestPath", dxmt_manifest);
    optional_string(&writer, "m12ManifestPath", m12_manifest);
    optional_string(&writer, "path", dxmt);
    optional_string(&writer, "m12Path", m12);
    ms_json_writer_key(&writer, "dxmt");
    ms_json_writer_object_begin(&writer);
    ms_json_writer_key(&writer, "current");
    ms_json_writer_bool(&writer, current);
    ms_json_writer_key(&writer, "filesReady");
    ms_json_writer_bool(&writer, ready);
    optional_string(&writer, "installedVersion", version);
    ms_json_writer_key(&writer, "requiredVersion");
    ms_json_writer_string(&writer, MS_DXMT_VERSION);
    optional_string(&writer, "manifestPath", dxmt_manifest);
    optional_string(&writer, "path", dxmt);
    ms_json_writer_object_end(&writer);
    ms_json_writer_key(&writer, "dxmt_m12");
    ms_json_writer_object_begin(&writer);
    ms_json_writer_key(&writer, "current");
    ms_json_writer_bool(&writer, m12_current);
    ms_json_writer_key(&writer, "filesReady");
    ms_json_writer_bool(&writer, m12_ready);
    optional_string(&writer, "installedVersion", m12_version);
    ms_json_writer_key(&writer, "requiredVersion");
    ms_json_writer_string(&writer, MS_DXMT_VERSION);
    optional_string(&writer, "manifestPath", m12_manifest);
    optional_string(&writer, "path", m12);
    ms_json_writer_object_end(&writer);
    ms_json_writer_object_end(&writer);
    result = ms_json_writer_take(&writer);
    free(dxmt);
    free(m12);
    free(dxmt_manifest);
    free(m12_manifest);
    free(version);
    free(m12_version);
    return result;
}

static char* setup_path(const char* home) {
    return join_path(home, "setup.json");
}

char* ms_setup_state_json(const char* metalsharp_home) {
    char* path = setup_path(metalsharp_home);
    ms_json* config = path == NULL ? NULL : read_json(path);
    char* runtime = runtime_status_json(metalsharp_home);
    bool saved_completed = get_bool(config, "completed", false);
    bool runtime_current = false;
    bool runtime_lib_ready = host_runtime_lib_ready(metalsharp_home);
    unsigned long long step = get_u64(config, "step", 0);
    char* device_name = get_string(config, "deviceName", "");
    bool api_key = get_bool(config, "steamApiKeySet", false);
    ms_json_writer writer;
    char* result;
    if (runtime != NULL) {
        char error[128];
        ms_json* runtime_value = ms_json_parse(runtime, strlen(runtime), error, sizeof(error));
        runtime_current = get_bool(runtime_value, "current", false) && get_bool(runtime_value, "m12Current", false) &&
                          runtime_lib_ready;
        ms_json_free(runtime_value);
    }
    ms_json_writer_init(&writer);
    ms_json_writer_object_begin(&writer);
    ms_json_writer_key(&writer, "ok");
    ms_json_writer_bool(&writer, true);
    ms_json_writer_key(&writer, "completed");
    ms_json_writer_bool(&writer, saved_completed && runtime_current);
    ms_json_writer_key(&writer, "savedCompleted");
    ms_json_writer_bool(&writer, saved_completed);
    ms_json_writer_key(&writer, "step");
    ms_json_writer_u64(&writer, step);
    ms_json_writer_key(&writer, "deviceName");
    ms_json_writer_string(&writer, device_name);
    ms_json_writer_key(&writer, "steamApiKeySet");
    ms_json_writer_bool(&writer, api_key);
    ms_json_writer_key(&writer, "runtimeMigrationRequired");
    ms_json_writer_bool(&writer, saved_completed && !runtime_current);
    ms_json_writer_key(&writer, "dxmtRuntime");
    ms_json_writer_raw(&writer, runtime == NULL ? "{}" : runtime);
    ms_json_writer_key(&writer, "metalsharpRuntimeLibReady");
    ms_json_writer_bool(&writer, runtime_lib_ready);
    ms_json_writer_object_end(&writer);
    result = ms_json_writer_take(&writer);
    free(path);
    free(runtime);
    free(device_name);
    ms_json_free(config);
    return result;
}

static void write_member(ms_json_writer* writer, const char* key, const ms_json* value) {
    char* serialized = ms_json_stringify(value);
    ms_json_writer_key(writer, key);
    ms_json_writer_raw(writer, serialized == NULL ? "null" : serialized);
    free(serialized);
}

char* ms_setup_save_json(const char* metalsharp_home, const unsigned char* body, size_t body_length, int* status) {
    char* path = setup_path(metalsharp_home);
    char* parent = path == NULL ? NULL : strdup(path);
    ms_json* existing = path == NULL ? NULL : read_json(path);
    ms_json* request = NULL;
    char error[128];
    ms_json_writer writer;
    size_t i;
    bool has_step = false, has_completed = false, has_device = false, has_key = false;
    unsigned long long step = 0;
    bool completed = false, api_key = false;
    char* device = NULL;
    char* result;
    if (status != NULL)
        *status = 500;
    if (parent == NULL)
        goto fail;
    {
        char* slash = strrchr(parent, '/');
        if (slash != NULL) {
            *slash = '\0';
            if (!mkdir_p(parent))
                goto fail;
        }
    }
    if (existing == NULL || ms_json_type_of(existing) != MS_JSON_OBJECT) {
        ms_json_free(existing);
        existing = NULL;
    }
    if (body != NULL && body_length > 0) {
        request = ms_json_parse((const char*)body, body_length, error, sizeof(error));
        if (request == NULL || ms_json_type_of(request) != MS_JSON_OBJECT) {
            ms_json_free(request);
            request = NULL;
        }
    }
    if (request != NULL) {
        const ms_json* value = ms_json_object_get(request, "step");
        long long parsed_step = 0;
        has_step = value != NULL && ms_json_as_i64(value, &parsed_step) && parsed_step >= 0;
        if (has_step)
            step = (unsigned long long)parsed_step;
        value = ms_json_object_get(request, "completed");
        has_completed = value != NULL && ms_json_as_bool(value, &completed);
        value = ms_json_object_get(request, "deviceName");
        has_device = ms_json_as_string(value, &device);
        value = ms_json_object_get(request, "steamApiKeySet");
        has_key = value != NULL && ms_json_as_bool(value, &api_key);
    }
    ms_json_writer_init(&writer);
    ms_json_writer_object_begin(&writer);
    for (i = 0; existing != NULL && i < ms_json_object_length(existing); ++i) {
        const char* key = ms_json_object_key_at(existing, i);
        const ms_json* value = ms_json_object_value_at(existing, i);
        if (has_step && strcmp(key, "step") == 0) {
            ms_json_writer_key(&writer, key);
            ms_json_writer_u64(&writer, step);
        } else if (has_completed && strcmp(key, "completed") == 0) {
            ms_json_writer_key(&writer, key);
            ms_json_writer_bool(&writer, completed);
        } else if (has_device && strcmp(key, "deviceName") == 0) {
            ms_json_writer_key(&writer, key);
            ms_json_writer_string(&writer, device);
        } else if (has_key && strcmp(key, "steamApiKeySet") == 0) {
            ms_json_writer_key(&writer, key);
            ms_json_writer_bool(&writer, api_key);
        } else
            write_member(&writer, key, value);
    }
    if (has_step && ms_json_object_get(existing, "step") == NULL) {
        ms_json_writer_key(&writer, "step");
        ms_json_writer_u64(&writer, step);
    }
    if (has_completed && ms_json_object_get(existing, "completed") == NULL) {
        ms_json_writer_key(&writer, "completed");
        ms_json_writer_bool(&writer, completed);
    }
    if (has_device && ms_json_object_get(existing, "deviceName") == NULL) {
        ms_json_writer_key(&writer, "deviceName");
        ms_json_writer_string(&writer, device);
    }
    if (has_key && ms_json_object_get(existing, "steamApiKeySet") == NULL) {
        ms_json_writer_key(&writer, "steamApiKeySet");
        ms_json_writer_bool(&writer, api_key);
    }
    ms_json_writer_object_end(&writer);
    result = ms_json_writer_take(&writer);
    if (result == NULL)
        goto fail;
    {
        FILE* file = fopen(path, "wb");
        if (file == NULL || fputs(result, file) < 0 || fclose(file) != 0) {
            if (file != NULL)
                fclose(file);
            free(result);
            result = NULL;
            goto fail;
        }
    }
    free(parent);
    free(path);
    ms_json_free(existing);
    ms_json_free(request);
    free(device);
    result = ms_setup_state_json(metalsharp_home);
    if (status != NULL)
        *status = result == NULL ? 500 : 200;
    return result;
fail:
    free(parent);
    free(path);
    ms_json_free(existing);
    ms_json_free(request);
    free(device);
    if (status != NULL)
        *status = 500;
    return strdup("{\"ok\":false,\"error\":\"failed to write setup state\"}");
}

char* ms_setup_device_name_json(void) {
    static const char* const adjectives[] = {"Swift", "Crimson", "Silent", "Bright", "Shadow", "Frost", "Ember",
                                             "Storm", "Lunar",   "Solar",  "Nova",   "Pixel",  "Cyber", "Iron",
                                             "Neon",  "Blaze",   "Drift",  "Pulse",  "Glitch", "Volt"};
    static const char* const nouns[] = {"Wolf",   "Falcon", "Tiger", "Raven", "Phoenix", "Cobra", "Panther",
                                        "Hawk",   "Lynx",   "Viper", "Fox",   "Bear",    "Eagle", "Shark",
                                        "Dragon", "Knight", "Blade", "Spark", "Forge",   "Core"};
    unsigned char random_bytes[4] = {0};
    unsigned int a, n;
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd >= 0) {
        (void)read(fd, random_bytes, sizeof(random_bytes));
        close(fd);
    } else {
        unsigned long seed = (unsigned long)getpid() ^ (unsigned long)time(NULL);
        random_bytes[0] = (unsigned char)seed;
        random_bytes[1] = (unsigned char)(seed >> 8);
        random_bytes[2] = (unsigned char)(seed >> 16);
        random_bytes[3] = (unsigned char)(seed >> 24);
    }
    a = (((unsigned int)random_bytes[0] << 16) | ((unsigned int)random_bytes[1] << 8) | random_bytes[2]) %
        (sizeof(adjectives) / sizeof(adjectives[0]));
    n = random_bytes[3] % (sizeof(nouns) / sizeof(nouns[0]));
    {
        char name[64];
        ms_json_writer writer;
        char* result;
        (void)snprintf(name, sizeof(name), "%s-%s", adjectives[a], nouns[n]);
        ms_json_writer_init(&writer);
        ms_json_writer_object_begin(&writer);
        ms_json_writer_key(&writer, "ok");
        ms_json_writer_bool(&writer, true);
        ms_json_writer_key(&writer, "name");
        ms_json_writer_string(&writer, name);
        ms_json_writer_object_end(&writer);
        result = ms_json_writer_take(&writer);
        return result;
    }
}

static bool host_runtime_installed(const char* home) {
    char* runtime = join_path(home, "runtime/host");
    char* manifest = runtime == NULL ? NULL : join_path(runtime, "manifest.json");
    char* abi = runtime == NULL ? NULL : join_path(runtime, "HostRuntimeABI.h");
    char* dylib = runtime == NULL ? NULL : join_path(runtime, "libmetalsharp_host_runtime.dylib");
    char* so = runtime == NULL ? NULL : join_path(runtime, "libmetalsharp_host_runtime.so");
    char* dll = runtime == NULL ? NULL : join_path(runtime, "metalsharp_host_runtime.dll");
    bool ok = file_nonempty(manifest) && file_nonempty(abi) &&
              (file_nonempty(dylib) || file_nonempty(so) || file_nonempty(dll));
    free(runtime);
    free(manifest);
    free(abi);
    free(dylib);
    free(so);
    free(dll);
    return ok;
}

static bool any_fixed_file(const char* const* paths, size_t count) {
    size_t i;
    for (i = 0; i < count; ++i)
        if (file_nonempty(paths[i]) || access(paths[i], F_OK) == 0)
            return true;
    return false;
}

static bool home_required_files_ready(const char* home, const char* const* relative_paths, size_t count) {
    for (size_t i = 0; i < count; i++) {
        char* path = join_path(home, relative_paths[i]);
        bool ready = path && file_nonempty(path);
        free(path);
        if (!ready)
            return false;
    }
    return true;
}

static bool home_required_dirs_ready(const char* home, const char* const* relative_paths, size_t count) {
    for (size_t i = 0; i < count; i++) {
        char* path = join_path(home, relative_paths[i]);
        struct stat info;
        bool ready = path && stat(path, &info) == 0 && S_ISDIR(info.st_mode);
        free(path);
        if (!ready)
            return false;
    }
    return true;
}

static bool fix_moltenvk_icd_library_path(const char* path, const char* library) {
    char* raw = setup_read_text(path);
    const char* marker;
    const char* colon;
    const char* opening;
    const char* closing;
    char* quoted;
    char* updated;
    FILE* file;
    size_t prefix_length;
    size_t suffix_length;
    bool ok = false;
    if (!raw)
        return false;
    marker = strstr(raw, "\"library_path\"");
    colon = marker ? strchr(marker, ':') : NULL;
    opening = colon ? strchr(colon, '\"') : NULL;
    closing = opening ? strchr(opening + 1, '\"') : NULL;
    quoted = ms_json_quote(library);
    if (!opening || !closing || !quoted) {
        free(raw);
        free(quoted);
        return false;
    }
    prefix_length = (size_t)(opening - raw);
    suffix_length = strlen(closing + 1);
    updated = malloc(prefix_length + strlen(quoted) + suffix_length + 1);
    if (updated) {
        memcpy(updated, raw, prefix_length);
        memcpy(updated + prefix_length, quoted, strlen(quoted));
        memcpy(updated + prefix_length + strlen(quoted), closing + 1, suffix_length + 1);
        file = fopen(path, "wb");
        if (file) {
            size_t length = prefix_length + strlen(quoted) + suffix_length;
            if (fwrite(updated, 1, length, file) == length)
                ok = fclose(file) == 0;
            else
                fclose(file);
        }
    }
    free(updated);
    free(quoted);
    free(raw);
    return ok;
}

static bool moltenvk_runtime_ready(const char* home) {
    const char* libraries[] = {
        "runtime/wine/lib/wine/x86_64-unix/libMoltenVK.dylib", "runtime/wine/lib/wine/x86_64-unix/libMoltenVK.1.dylib",
        "runtime/wine/lib/moltenvk-vkmt/libMoltenVK.dylib", "runtime/wine/lib/moltenvk-vkmt/libMoltenVK.1.dylib"};
    char *lane = join_path(home, "runtime/wine/lib/moltenvk-vkmt/MoltenVK_icd.json"),
         *runtime = join_path(home, "runtime/wine/etc/vulkan/icd.d/MoltenVK_icd.json"),
         *library = join_path(home, libraries[0]);
    bool ready = true;
    for (size_t i = 0; i < sizeof(libraries) / sizeof(libraries[0]); i++) {
        char* path = join_path(home, libraries[i]);
        ready = ready && path && sha256_matches(path, MS_MOLTENVK_LIBRARY_SHA256);
        free(path);
    }
    ready = ready && lane && sha256_matches(lane, MS_MOLTENVK_LANE_ICD_SHA256) && runtime;
    if (ready) {
        char* raw = setup_read_text(runtime);
        char parse_error[128];
        ms_json* value = raw ? ms_json_parse(raw, strlen(raw), parse_error, sizeof(parse_error)) : NULL;
        const ms_json* icd = value ? ms_json_object_get(value, "ICD") : NULL;
        char* path = icd ? NULL : NULL;
        bool portability = false;
        const ms_json* lib_path = icd ? ms_json_object_get(icd, "library_path") : NULL;
        if (!value || ms_json_type_of(value) != MS_JSON_OBJECT || !ms_json_object_get(value, "file_format_version") ||
            !ms_json_as_string(ms_json_object_get(value, "file_format_version"), &path) || strcmp(path, "1.0.0") != 0)
            ready = false;
        free(path);
        path = NULL;
        if (!icd || !ms_json_as_string(ms_json_object_get(icd, "api_version"), &path) || strcmp(path, "1.4.0") != 0)
            ready = false;
        free(path);
        path = NULL;
        if (!icd || !ms_json_as_bool(ms_json_object_get(icd, "is_portability_driver"), &portability) || !portability)
            ready = false;
        if (!lib_path || !ms_json_as_string(lib_path, &path) || !library)
            ready = false;
        else if (strcmp(path, library) != 0 && !fix_moltenvk_icd_library_path(runtime, library))
            ready = false;
        free(path);
        ms_json_free(value);
        free(raw);
    }
    free(lane);
    free(runtime);
    free(library);
    return ready;
}

static bool command_available(const char* name) {
    const char* path = getenv("PATH");
    const char* fixed_dirs[] = {"/opt/homebrew/bin", "/usr/local/bin", "/usr/bin", "/bin"};
    for (size_t i = 0; i < sizeof(fixed_dirs) / sizeof(fixed_dirs[0]); i++) {
        char* candidate = join_path(fixed_dirs[i], name);
        bool found = candidate && access(candidate, X_OK) == 0;
        free(candidate);
        if (found)
            return true;
    }
    char* copy;
    char* part;
    char* save = NULL;
    if (path == NULL)
        return false;
    copy = strdup(path);
    if (copy == NULL)
        return false;
    for (part = strtok_r(copy, ":", &save); part != NULL; part = strtok_r(NULL, ":", &save)) {
        char* candidate = join_path(part, name);
        if (candidate != NULL && access(candidate, X_OK) == 0) {
            free(candidate);
            free(copy);
            return true;
        }
        free(candidate);
    }
    free(copy);
    return false;
}

static bool xcode_cli_functional(void) {
    int input[2];
    pid_t pid;
    int wait_status;
    const char source[] = "int main(void) { return 0; }\n";
    if (!command_available("clang") || pipe(input) != 0)
        return false;
    pid = fork();
    if (pid == 0) {
        dup2(input[0], STDIN_FILENO);
        close(input[0]);
        close(input[1]);
        execl("/usr/bin/clang", "clang", "-x", "c", "-c", "-o", "/dev/null", "-", (char*)NULL);
        _exit(127);
    }
    close(input[0]);
    if (pid < 0) {
        close(input[1]);
        return false;
    }
    (void)write(input[1], source, sizeof(source) - 1);
    close(input[1]);
    while (waitpid(pid, &wait_status, 0) < 0 && errno == EINTR) {
    }
    return WIFEXITED(wait_status) && WEXITSTATUS(wait_status) == 0;
}

static bool install_xcode_cli(void) {
    pid_t pid;
    int wait_status;
    if (xcode_cli_functional())
        return true;
    pid = fork();
    if (pid == 0) {
        execl("/usr/bin/xcode-select", "xcode-select", "--install", (char*)NULL);
        _exit(127);
    }
    if (pid > 0)
        while (waitpid(pid, &wait_status, 0) < 0 && errno == EINTR) {
        }
    for (unsigned i = 0; i < 120; i++) {
        sleep(5);
        if (xcode_cli_functional())
            return true;
    }
    pid = fork();
    if (pid == 0) {
        execl("/usr/sbin/softwareupdate", "softwareupdate", "--install", "*Command Line Tools*", (char*)NULL);
        _exit(127);
    }
    if (pid > 0)
        while (waitpid(pid, &wait_status, 0) < 0 && errno == EINTR) {
        }
    return xcode_cli_functional();
}

static bool install_homebrew(void) {
    const char* configured = getenv("METALSHARP_HOMEBREW_INSTALLER");
    char* script = configured && access(configured, R_OK) == 0 ? strdup(configured) : NULL;
    pid_t pid;
    int wait_status;
    if (command_available("brew")) {
        free(script);
        return true;
    }
    if (script == NULL)
        script = find_setup_source("scripts/tools/install-homebrew.sh");
    if (script == NULL)
        script = find_setup_source("tools/install-homebrew.sh");
    if (script == NULL)
        return false;
    pid = fork();
    if (pid == 0) {
        execl("/bin/bash", "bash", script, (char*)NULL);
        _exit(127);
    }
    free(script);
    if (pid < 0)
        return false;
    while (waitpid(pid, &wait_status, 0) < 0 && errno == EINTR) {
    }
    return WIFEXITED(wait_status) && WEXITSTATUS(wait_status) == 0 && command_available("brew");
}

static void dependency_begin(ms_json_writer* writer, const char* id, const char* name, const char* desc, bool installed,
                             bool required, const char* install_command) {
    ms_json_writer_object_begin(writer);
    ms_json_writer_key(writer, "id");
    ms_json_writer_string(writer, id);
    ms_json_writer_key(writer, "name");
    ms_json_writer_string(writer, name);
    ms_json_writer_key(writer, "desc");
    ms_json_writer_string(writer, desc);
    ms_json_writer_key(writer, "installed");
    ms_json_writer_bool(writer, installed);
    ms_json_writer_key(writer, "required");
    ms_json_writer_bool(writer, required);
    ms_json_writer_key(writer, "installCmd");
    ms_json_writer_string(writer, install_command);
}

char* ms_setup_dependencies_json(const char* metalsharp_home) {
    const char* homebrew_paths[] = {"/opt/homebrew/bin/brew", "/usr/local/bin/brew"};
    const char* steam_paths[] = {"/Applications/Steam.app/Contents/MacOS/steam_osx"};
    const char* moltenvk_paths[] = {"/opt/homebrew/etc/vulkan/icd.d/MoltenVK_icd.json"};
    char* wine = join_path(metalsharp_home, "runtime/wine/bin/wine");
    char* metalsharp_wine = join_path(metalsharp_home, "runtime/wine/bin/metalsharp-wine");
    char* runtime_json = runtime_status_json(metalsharp_home);
    char error[128];
    ms_json* runtime =
        runtime_json == NULL ? NULL : ms_json_parse(runtime_json, strlen(runtime_json), error, sizeof(error));
    bool homebrew =
        any_fixed_file(homebrew_paths, sizeof(homebrew_paths) / sizeof(homebrew_paths[0])) || command_available("brew");
    bool xcode = access("/usr/bin/clang", X_OK) == 0 || access("/usr/bin/xcodebuild", X_OK) == 0 ||
                 command_available("clang") || command_available("xcodebuild");
    bool rosetta = access("/Library/Apple/System/Library/LaunchDaemons/com.apple.oahd.plist", F_OK) == 0 ||
                   system("/usr/bin/pgrep -q oahd >/dev/null 2>&1") == 0;
    bool steam = any_fixed_file(steam_paths, sizeof(steam_paths) / sizeof(steam_paths[0]));
    bool moltenvk = any_fixed_file(moltenvk_paths, sizeof(moltenvk_paths) / sizeof(moltenvk_paths[0]));
    bool wine_ready = file_nonempty(wine) || file_nonempty(metalsharp_wine);
    bool host_ready = host_runtime_installed(metalsharp_home);
    bool dxmt_ready = get_bool(runtime, "current", false);
    bool m12_ready = get_bool(runtime, "m12Current", false);
    bool mono = access("/opt/homebrew/bin/mono", F_OK) == 0 || access("/usr/local/bin/mono", F_OK) == 0 ||
                command_available("mono");
    ms_json_writer writer;
    char* result;
    ms_json_writer_init(&writer);
    ms_json_writer_object_begin(&writer);
    ms_json_writer_key(&writer, "ok");
    ms_json_writer_bool(&writer, true);
    ms_json_writer_key(&writer, "allInstalled");
    ms_json_writer_bool(&writer, homebrew && rosetta && xcode && wine_ready && host_ready && dxmt_ready && m12_ready);
    ms_json_writer_key(&writer, "platform");
    ms_json_writer_string(&writer, "macos");
    ms_json_writer_key(&writer, "dependencies");
    ms_json_writer_array_begin(&writer);
    dependency_begin(
        &writer, "homebrew", "Homebrew", "Package manager — required to install other dependencies", homebrew, true,
        "bash scripts/tools/install-homebrew.sh");
    ms_json_writer_object_end(&writer);
    dependency_begin(&writer, "xcode_cli", "Xcode Command Line Tools",
                     "Provides clang for building native shims (CSteamworks, gdiplus stub)", xcode, true,
                     "xcode-select --install");
    ms_json_writer_object_end(&writer);
    dependency_begin(&writer, "rosetta", "Rosetta 2",
                     "x86_64 translation layer — needed for 32-bit Windows games and x86 mono", rosetta, true,
                     "softwareupdate --install-rosetta --agree-to-license");
    ms_json_writer_object_end(&writer);
    dependency_begin(&writer, "metalsharp_wine", "MetalSharp Wine",
                     "From-source Wine 11.5 with DXMT Metal D3D11, gnutls TLS, MoltenVK. Runs Windows Steam and "
                     "launches games with native Metal rendering.",
                     wine_ready, true, "metalsharp-setup-wine");
    ms_json_writer_object_end(&writer);
    dependency_begin(&writer, "metalsharp_host_runtime", "MetalSharp Host Runtime ABI",
                     "Bottle-aware native host service ABI used by Wine shims and launch routes.", host_ready, true,
                     "metalsharp-setup-host-runtime");
    ms_json_writer_object_end(&writer);
    dependency_begin(&writer, "dxmt_runtime", "DXMT M9-M11 Runtime",
                     "Bundled D3D9/D3D10/D3D11-to-Metal runtime (0.60.0-m12-isolated-surface-v1) staged under "
                     "runtime/wine/lib/dxmt.",
                     dxmt_ready, true, "metalsharp-setup-dxmt");
    if (runtime != NULL) {
        char* status = ms_json_stringify(ms_json_object_get(runtime, "dxmt"));
        ms_json_writer_key(&writer, "status");
        ms_json_writer_raw(&writer, status);
        free(status);
    }
    ms_json_writer_object_end(&writer);
    dependency_begin(&writer, "dxmt_m12_runtime", "DXMT M12 Runtime",
                     "Isolated D3D12-to-Metal runtime staged under runtime/wine/lib/dxmt_m12 with its own DLLs and "
                     "winemetal.so sidecars.",
                     m12_ready, true, "metalsharp-setup-dxmt-m12");
    {
        char* m12_path = join_path(metalsharp_home, "runtime/wine/lib/dxmt_m12");
        ms_json_writer_key(&writer, "path");
        ms_json_writer_string(&writer, m12_path ? m12_path : "");
        free(m12_path);
    }
    if (runtime != NULL) {
        char* status = ms_json_stringify(ms_json_object_get(runtime, "dxmt_m12"));
        ms_json_writer_key(&writer, "status");
        ms_json_writer_raw(&writer, status);
        free(status);
    }
    ms_json_writer_object_end(&writer);
    dependency_begin(&writer, "mono", "Mono Runtime (arm64)", "Required for Terraria and other arm64 FNA/XNA games",
                     mono, false, "brew install mono");
    ms_json_writer_object_end(&writer);
    dependency_begin(&writer, "moltenvk", "MoltenVK", "Vulkan→Metal translation. Optional fallback graphics backend.",
                     moltenvk, false, "brew install molten-vk");
    ms_json_writer_object_end(&writer);
    dependency_begin(&writer, "steam", "Steam Client (macOS)",
                     "Provides native macOS libraries (libsteam_api.dylib) for FNA games. Install Terraria for macOS "
                     "to get the best compatibility.",
                     steam, false, "https://store.steampowered.com/about/");
    ms_json_writer_object_end(&writer);
    ms_json_writer_array_end(&writer);
    ms_json_writer_object_end(&writer);
    result = ms_json_writer_take(&writer);
    free(wine);
    free(metalsharp_wine);
    free(runtime_json);
    ms_json_free(runtime);
    return result;
}

char* ms_setup_agility_versions_json(void) {
    static const unsigned retail_sdk[] = {4, 600, 602, 606, 608, 610, 611, 613, 614, 615, 616, 618, 619};
    static const char* const retail_package[] = {"1.4.10",  "1.600.10", "1.602.4", "1.606.4", "1.608.3",
                                                 "1.610.4", "1.611.2",  "1.613.3", "1.614.1", "1.615.1",
                                                 "1.616.1", "1.618.5",  "1.619.3"};
    static const unsigned preview_sdk[] = {700, 706, 710, 711, 714, 715, 716, 717, 719, 720, 721};
    static const char* const preview_package[] = {"1.700.10-preview", "1.706.4-preview", "1.710.0-preview",
                                                  "1.711.3-preview",  "1.714.0-preview", "1.715.0-preview",
                                                  "1.716.1-preview",  "1.717.1-preview", "1.719.1-preview",
                                                  "1.720.0-preview",  "1.721.0-preview"};
    ms_json_writer writer;
    char* result;
    size_t i;
    ms_json_writer_init(&writer);
    ms_json_writer_object_begin(&writer);
    ms_json_writer_key(&writer, "default");
    ms_json_writer_string(&writer, "1.619.3");
    ms_json_writer_key(&writer, "retail");
    ms_json_writer_array_begin(&writer);
    for (i = 0; i < sizeof(retail_sdk) / sizeof(retail_sdk[0]); ++i) {
        ms_json_writer_object_begin(&writer);
        ms_json_writer_key(&writer, "sdk_version");
        ms_json_writer_u64(&writer, retail_sdk[i]);
        ms_json_writer_key(&writer, "package_version");
        ms_json_writer_string(&writer, retail_package[i]);
        ms_json_writer_key(&writer, "channel");
        ms_json_writer_string(&writer, "retail");
        ms_json_writer_object_end(&writer);
    }
    ms_json_writer_array_end(&writer);
    ms_json_writer_key(&writer, "preview");
    ms_json_writer_array_begin(&writer);
    for (i = 0; i < sizeof(preview_sdk) / sizeof(preview_sdk[0]); ++i) {
        ms_json_writer_object_begin(&writer);
        ms_json_writer_key(&writer, "sdk_version");
        ms_json_writer_u64(&writer, preview_sdk[i]);
        ms_json_writer_key(&writer, "package_version");
        ms_json_writer_string(&writer, preview_package[i]);
        ms_json_writer_key(&writer, "channel");
        ms_json_writer_string(&writer, "preview");
        ms_json_writer_object_end(&writer);
    }
    ms_json_writer_array_end(&writer);
    ms_json_writer_object_end(&writer);
    result = ms_json_writer_take(&writer);
    return result;
}

static void refresh_installing(void);

char* ms_setup_install_progress_json(const char* metalsharp_home) {
    refresh_installing();
    char* path = join_path(metalsharp_home, "install_progress.json");
    char* text = path == NULL ? NULL : read_file(path, NULL);
    free(path);
    return text == NULL
               ? strdup("{\"step\":0,\"total\":0,\"current\":\"\",\"status\":\"idle\",\"log\":\"\",\"error\":null}")
               : text;
}

static atomic_bool g_installing = false;
static pid_t g_install_pid = 0;

static void refresh_installing(void) {
    int wait_status;
    if (g_install_pid > 0 && waitpid(g_install_pid, &wait_status, WNOHANG) == g_install_pid) {
        g_install_pid = 0;
        atomic_store(&g_installing, false);
    }
}

static void write_install_progress(const char* home, unsigned step, unsigned total, const char* current,
                                   const char* state, const char* log, const char* error) {
    char* path = join_path(home, "install_progress.json");
    FILE* file;
    if (!path)
        return;
    file = fopen(path, "wb");
    if (file) {
        if (error)
            fprintf(file,
                    "{\"step\":%u,\"total\":%u,\"current\":\"%s\",\"status\":\"%s\",\"log\":\"%s\",\"error\":\"%s\"}",
                    step, total, current, state, log, error);
        else
            fprintf(file,
                    "{\"step\":%u,\"total\":%u,\"current\":\"%s\",\"status\":\"%s\",\"log\":\"%s\",\"error\":null}",
                    step, total, current, state, log);
        fclose(file);
    }
    free(path);
}

static void* install_parent_monitor(void* unused) {
    pid_t parent = getppid();
    (void)unused;
    for (;;) {
        sleep(1);
        if (getppid() != parent)
            _exit(0);
    }
    return NULL;
}

static void run_install_all_worker(const char* home) {
    pthread_t parent_monitor;
    const unsigned total = 17;
    if (pthread_create(&parent_monitor, NULL, install_parent_monitor, NULL) == 0)
        pthread_detach(parent_monitor);
    char missing[256] = {0};
    bool ok = true;
    char* runtime;
    char error[128];
    ms_json* json;
    bool dxmt;
    write_install_progress(home, 0, total, "Starting...", "starting", "Verifying prerequisites...", NULL);
    if (!command_available("tar")) {
        snprintf(missing + strlen(missing), sizeof(missing) - strlen(missing), "%star", missing[0] ? ", " : "");
        ok = false;
    }
    if (!command_available("curl")) {
        snprintf(missing + strlen(missing), sizeof(missing) - strlen(missing), "%scurl", missing[0] ? ", " : "");
        ok = false;
    }
    if (!ok) {
        write_install_progress(home, 0, total, "Prerequisites", "error", "Required prerequisites are missing", missing);
        _exit(0);
    }

    write_install_progress(home, 1, total, "Homebrew", "installing", "Installing Homebrew...", NULL);
    if (!install_homebrew()) {
        write_install_progress(home, 1, total, "Homebrew", "error", "Homebrew installation failed",
                               "run tools/install-homebrew.sh to retry");
        _exit(0);
    }
    write_install_progress(home, 1, total, "Homebrew Packages", "installing",
                           "Installing GameJolt archive and icon tools...", NULL);
    if ((!command_available("wrestool") || !command_available("icotool")) && !run_brew_install("icoutils")) {
        write_install_progress(home, 1, total, "Homebrew Packages", "error", "GameJolt icon tools installation failed",
                               "brew install icoutils failed");
        _exit(0);
    }
    if (!command_available("unar") && !run_brew_install("unar")) {
        write_install_progress(home, 1, total, "Homebrew Packages", "error", "RAR extraction tool installation failed",
                               "brew install unar failed");
        _exit(0);
    }
    write_install_progress(home, 1, total, "Homebrew Packages", "done", "Homebrew and GameJolt tools ready", NULL);

    write_install_progress(home, 2, total, "System Tools", "installing", "Checking Xcode Command Line Tools...", NULL);
    if (!install_xcode_cli()) {
        write_install_progress(home, 2, total, "System Tools", "error", "Xcode Command Line Tools installation failed",
                               "install manually with: xcode-select --install");
        _exit(0);
    }
    write_install_progress(home, 2, total, "System Tools", "done", "System tools ready", NULL);
    write_install_progress(home, 3, total, "Rosetta 2", "installing", "Checking Rosetta 2...", NULL);
    if (access("/Library/Apple/System/Library/LaunchDaemons/com.apple.oahd.plist", F_OK) != 0 &&
        !command_available("oahd")) {
        pid_t rosetta_pid = fork();
        int rosetta_status = 0;
        if (rosetta_pid == 0) {
            execl("/usr/sbin/softwareupdate", "softwareupdate", "--install-rosetta", "--agree-to-license", (char*)NULL);
            _exit(127);
        }
        if (rosetta_pid < 0)
            rosetta_status = -1;
        else
            while (waitpid(rosetta_pid, &rosetta_status, 0) < 0 && errno == EINTR) {
            }
        if (rosetta_pid < 0 || !WIFEXITED(rosetta_status) || WEXITSTATUS(rosetta_status) != 0) {
            write_install_progress(home, 3, total, "Rosetta 2", "error", "Rosetta 2 installation failed",
                                   "softwareupdate --install-rosetta failed");
            _exit(0);
        }
    }
    write_install_progress(home, 3, total, "Rosetta 2", "done", "Rosetta 2 ready", NULL);

    write_install_progress(home, 4, total, "Extract Tools (zstd)", "installing", "Checking zstd...", NULL);
    if (!command_available("zstd") && (!command_available("brew") || !run_brew_install("zstd"))) {
        write_install_progress(home, 4, total, "Extract Tools (zstd)", "error",
                               "zstd installation failed", "brew install zstd failed");
        _exit(0);
    }
    write_install_progress(home, 4, total, "Extract Tools (zstd)", "done", "zstd ready", NULL);
    {
        const char* bundles[] = {"metalsharp-runtime.tar.zst",       "metalsharp-graphics-dll.tar.zst",
                                 "metalsharp-assets.tar.zst",        "fnalibs.tar.zst",
                                 "metalsharp-scripts-tools.tar.zst", "metalsharp-steam.tar.zst"};
        for (size_t i = 0; i < sizeof(bundles) / sizeof(bundles[0]); i++) {
            char* archive = find_bundle_archive(home, bundles[i]);
            bool available = archive != NULL;
            free(archive);
            if (!available) {
                write_install_progress(home, 5, total, "Runtime Bundle Downloads", "downloading",
                                       "Downloading required runtime bundles...", NULL);
                available = download_bundle_archive(home, bundles[i]);
            }
            if (!available) {
                write_install_progress(home, 5, total, "Runtime Bundle Downloads", "error",
                                       "Required runtime bundle could not be downloaded", bundles[i]);
                _exit(0);
            }
        }
    }

    write_install_progress(home, 5, total, "Runtime Bundle Downloads", "installing", "Locating runtime bundles...",
                           NULL);
    {
        char* archive = find_bundle_archive(home, "metalsharp-runtime.tar.zst");
        char* existing_wine = join_path(home, "runtime/wine/bin/metalsharp-wine");
        char* existing_host = join_path(home, "runtime/host/manifest.json");
        bool already_ready = file_nonempty(existing_wine) && file_nonempty(existing_host);
        if (archive) {
            if (!extract_bundle_archive(home, archive)) {
                write_install_progress(home, 5, total, "Runtime Bundle Downloads", "error",
                                       "Failed to extract the MetalSharp runtime bundle",
                                       "runtime bundle extraction failed");
                free(archive);
                free(existing_wine);
                free(existing_host);
                _exit(0);
            }
            free(archive);
            write_install_progress(home, 5, total, "Runtime Bundle Downloads", "done", "Runtime bundle extracted",
                                   NULL);
        } else if (already_ready) {
            write_install_progress(home, 5, total, "Runtime Bundle Downloads", "done", "Runtime bundle already present",
                                   NULL);
        } else {
            write_install_progress(home, 5, total, "Runtime Bundle Downloads", "error",
                                   "Required runtime bundle is missing", "missing metalsharp-runtime.tar.zst");
            free(existing_wine);
            free(existing_host);
            _exit(0);
        }
        free(existing_wine);
        free(existing_host);
    }
    clear_runtime_quarantine(home);
    write_install_progress(home, 6, total, "Runtime Assets", "installing", "Checking runtime assets...", NULL);
    const char* runtime_files[] = {"runtime/wine/bin/metalsharp-wine", "runtime/host/manifest.json",
                                   "runtime/metalsharp-backend",
                                   "runtime/wine/lib/metalsharp/x86_64-windows/metalsharp_ntdll_hook.dll",
                                   "runtime/wine/lib/metalsharp/i386-windows/metalsharp_ntdll_hook.dll"};
    char* wine = join_path(home, "runtime/wine/bin/metalsharp-wine");
    char* host = join_path(home, "runtime/host/manifest.json");
    if (!home_required_files_ready(home, runtime_files, sizeof(runtime_files) / sizeof(runtime_files[0]))) {
        write_install_progress(home, 6, total, "Runtime Assets", "error",
                               "Runtime assets are missing; install or download the MetalSharp runtime bundle first",
                               "missing runtime assets");
        free(wine);
        free(host);
        _exit(0);
    }
    {
        pid_t wine_pid = fork();
        int wine_status = 0;
        if (wine_pid == 0) {
            execl(wine, wine, "--version", (char*)NULL);
            _exit(127);
        }
        if (wine_pid < 0)
            wine_status = -1;
        else
            while (waitpid(wine_pid, &wine_status, 0) < 0 && errno == EINTR) {
            }
        if (wine_pid < 0 || !WIFEXITED(wine_status) || WEXITSTATUS(wine_status) != 0) {
            write_install_progress(home, 6, total, "Runtime Assets", "error", "Wine runtime validation failed",
                                   "metalsharp-wine --version failed");
            free(wine);
            free(host);
            _exit(0);
        }
    }
    free(wine);
    free(host);
    write_install_progress(home, 6, total, "Runtime Assets", "done", "Runtime assets ready", NULL);
    if (!moltenvk_runtime_ready(home)) {
        write_install_progress(home, 7, total, "VKD3D MoltenVK Runtime", "error",
                               "VKD3D/MoltenVK runtime is incomplete", "missing or invalid MoltenVK runtime artifacts");
        _exit(0);
    }
    write_install_progress(home, 7, total, "VKD3D MoltenVK Runtime", "done", "VKD3D/MoltenVK runtime ready", NULL);
    if (!host_runtime_installed(home)) {
        write_install_progress(home, 8, total, "Host Runtime ABI", "error", "Host runtime ABI is incomplete",
                               "missing host runtime ABI artifacts");
        _exit(0);
    }
    write_install_progress(home, 8, total, "Host Runtime ABI", "done", "Host runtime ABI ready", NULL);
    {
        const char* const mapping[][2] = {{"assets/mono-x86", "runtime/mono-x86"},
                                          {"assets/mono-arm64", "runtime/mono-arm64"},
                                          {"assets/dxvk-1.10.3", "runtime/dxvk-1.10.3"},
                                          {"assets/goldberg", "runtime/goldberg"},
                                          {"assets/shims", "runtime/shims"},
                                          {"assets/fnalibs", "runtime/fnalibs"},
                                          {"assets/fna-kickstart", "runtime/fna-kickstart"},
                                          {"assets/wine/etc", "runtime/wine/etc"},
                                          {"assets/shader-cache", "shader-cache"}};
        const char* support_dirs[] = {"runtime/mono-x86", "runtime/mono-arm64", "runtime/shims", "runtime/fnalibs",
                                      "runtime/fna-kickstart"};
        write_install_progress(home, 9, total, "Support Assets", "installing", "Staging support assets...", NULL);
        char* support_archive = find_bundle_archive(home, "metalsharp-assets.tar.zst");
        bool support_ready =
            home_required_dirs_ready(home, support_dirs, sizeof(support_dirs) / sizeof(support_dirs[0]));
        bool support_ok = support_archive &&
                          (support_ready || extract_split_bundle(home, "metalsharp-assets.tar.zst", mapping,
                                                                 sizeof(mapping) / sizeof(mapping[0]))) &&
                          home_required_dirs_ready(home, support_dirs, sizeof(support_dirs) / sizeof(support_dirs[0]));
        free(support_archive);
        if (!support_ok) {
            write_install_progress(home, 9, total, "Support Assets", "error",
                                   "Support asset bundle is missing or incomplete",
                                   "missing or invalid metalsharp-assets.tar.zst");
            _exit(0);
        }
    }
    write_install_progress(home, 9, total, "Support Assets", "done", "Support assets ready", NULL);
    {
        const char* const mapping[][2] = {{"scripts/tools", "scripts/tools"}};
        write_install_progress(home, 10, total, "Scripts and Tools", "installing", "Staging scripts and tools...", NULL);
        char* scripts_archive = find_bundle_archive(home, "metalsharp-scripts-tools.tar.zst");
        bool scripts_ready = home_required_dirs_ready(home, (const char*[]){"scripts/tools"}, 1);
        bool scripts_ok =
            scripts_archive &&
            (scripts_ready || extract_split_bundle(home, "metalsharp-scripts-tools.tar.zst", mapping, 1)) &&
            home_required_dirs_ready(home, (const char*[]){"scripts/tools"}, 1);
        free(scripts_archive);
        if (!scripts_ok) {
            write_install_progress(home, 10, total, "Scripts and Tools", "error",
                                   "Scripts/tools bundle is missing or incomplete",
                                   "missing or invalid metalsharp-scripts-tools.tar.zst");
            _exit(0);
        }
    }
    write_install_progress(home, 10, total, "Scripts and Tools", "done", "Scripts and tools ready", NULL);

    write_install_progress(home, 11, total, "DXMT Graphics Runtimes", "installing", "Extracting graphics runtimes...",
                           NULL);
    {
        char* archive = find_bundle_archive(home, "metalsharp-graphics-dll.tar.zst");
        char* temp = NULL;
        bool graphics_ok = true;
        if (archive) {
            char template_path[PATH_MAX];
            snprintf(template_path, sizeof(template_path), "%s/.graphics-extract-%ld", home, (long)getpid());
            remove_path_tree(template_path);
            if (mkdir(template_path, 0700) != 0 && errno != EEXIST)
                temp = NULL;
            else
                temp = strdup(template_path);
            if (!temp || !extract_archive_to(temp, archive)) {
                graphics_ok = false;
            } else {
                char *src_dxmt = join_path(temp, "Graphics/dll/dxmt"),
                     *src_m12 = join_path(temp, "Graphics/dll/dxmt-m12"),
                     *src_dxvk = join_path(temp, "Graphics/dll/dxvk"),
                     *src_vkd3d = join_path(temp, "Graphics/dll/vkd3d-proton"),
                     *dst_dxmt = join_path(home, "runtime/wine/lib/dxmt"),
                     *dst_m12 = join_path(home, "runtime/wine/lib/dxmt_m12"), *dst_dxvk = join_path(home, "vkd3d/dxvk"),
                     *dst_vkd3d = join_path(home, "vkd3d/vkd3d-proton");
                struct stat dxvk_info, vkd3d_info;
                bool has_dxvk = src_dxvk && stat(src_dxvk, &dxvk_info) == 0 && S_ISDIR(dxvk_info.st_mode);
                bool has_vkd3d = src_vkd3d && stat(src_vkd3d, &vkd3d_info) == 0 && S_ISDIR(vkd3d_info.st_mode);
                graphics_ok = src_dxmt && src_m12 && dst_dxmt && dst_m12 &&
                              copy_directory_contents(src_dxmt, dst_dxmt) &&
                              copy_directory_contents(src_m12, dst_m12) &&
                              (!has_dxvk || (dst_dxvk && copy_directory_contents(src_dxvk, dst_dxvk))) &&
                              (!has_vkd3d || (dst_vkd3d && copy_directory_contents(src_vkd3d, dst_vkd3d))) &&
                              write_dxmt_manifest(dst_dxmt) && write_dxmt_manifest(dst_m12);
                free(src_dxmt);
                free(src_m12);
                free(src_dxvk);
                free(src_vkd3d);
                free(dst_dxmt);
                free(dst_m12);
                free(dst_dxvk);
                free(dst_vkd3d);
            }
            if (temp)
                remove_path_tree(temp);
            free(archive);
        } else {
            graphics_ok = false;
        }
        if (!graphics_ok) {
            write_install_progress(home, 11, total, "DXMT Graphics Runtimes", "error",
                                   "DXMT graphics bundle is missing or could not be staged",
                                   "missing or invalid metalsharp-graphics-dll.tar.zst");
            _exit(0);
        }
    }
    runtime = runtime_status_json(home);
    json = runtime ? ms_json_parse(runtime, strlen(runtime), error, sizeof(error)) : NULL;
    dxmt = get_bool(json, "current", false) && get_bool(json, "m12Current", false);
    free(runtime);
    ms_json_free(json);
    if (!dxmt) {
        write_install_progress(home, 11, total, "DXMT Graphics Runtimes", "error", "DXMT runtimes are not ready",
                               "missing DXMT runtime");
        _exit(0);
    }
    write_install_progress(home, 11, total, "DXMT Graphics Runtimes", "done", "DXMT runtimes ready", NULL);
    {
        const char* goldberg[] = {"runtime/goldberg/x86/steam_api.dll", "runtime/goldberg/x64/steam_api64.dll"};
        if (!home_required_files_ready(home, goldberg, 2)) {
            write_install_progress(home, 12, total, "Goldberg Steam Emulator", "error",
                                   "Goldberg Steam emulator is incomplete", "missing Goldberg DLLs");
            _exit(0);
        }
    }
    write_install_progress(home, 12, total, "Goldberg Steam Emulator", "done", "Goldberg Steam emulator ready", NULL);
    {
        char* source = join_path(home, "runtime/shims/libsteam_api.dylib");
        char* destination = join_path(home, "runtime/steam-bridge/libsteam_api.dylib");
        if (source && destination && file_nonempty(source) && !file_nonempty(destination))
            (void)copy_file_path(source, destination);
        free(source);
        free(destination);
    }
    write_install_progress(home, 13, total, "Steam Bridge Shim", "done", "Steam bridge check complete", NULL);
    {
        char* source = join_path(home, "scripts/tools/configs/mtsp-rules.toml");
        char* destination = join_path(home, "configs/mtsp-rules.toml");
        if ((!source || !file_nonempty(source)) && access("configs/mtsp-rules.toml", R_OK) == 0) {
            free(source);
            source = strdup("configs/mtsp-rules.toml");
        }
        if (source && destination && file_nonempty(source))
            (void)copy_file_path(source, destination);
        free(source);
        free(destination);
    }
    write_install_progress(home, 14, total, "Pipeline Rules", "done", "Pipeline rules staged", NULL);
    {
        const char* names[] = {"terraria-mono.config", "celeste-x86-mono.config", "stardew-mono.config",
                               "generic-fna-mono.config"};
        for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
            char* source_dir = join_path(home, "scripts/tools/configs");
            char* source = source_dir ? join_path(source_dir, names[i]) : NULL;
            char* destination_dir = join_path(home, "configs");
            char* destination = destination_dir ? join_path(destination_dir, names[i]) : NULL;
            if (source && destination && file_nonempty(source))
                (void)copy_file_path(source, destination);
            free(source_dir);
            free(source);
            free(destination_dir);
            free(destination);
        }
    }
    write_install_progress(home, 15, total, "Mono Configs", "done", "Mono configuration staged", NULL);
    if (!home_required_files_ready(home, (const char*[]){"runtime/mono-arm64/bin/mono"}, 1) &&
        !command_available("mono"))
        (void)run_brew_install("mono");
    if (!home_required_files_ready(home, (const char*[]){"runtime/mono-arm64/bin/mono"}, 1) &&
        !command_available("mono")) {
        write_install_progress(home, 16, total, "Runtime Support", "error", "Mono arm64 runtime is incomplete",
                               "missing runtime/mono-arm64/bin/mono");
        _exit(0);
    }
    write_install_progress(home, 16, total, "Runtime Support", "done", "Runtime support ready", NULL);
    precompile_fna_shims(home);
    write_install_progress(home, 17, total, "FNA Shim Precompile", "done", "FNA shim precompile complete", NULL);
    write_install_progress(home, total, total, "Complete", "complete", "All assets installed!", NULL);
    _exit(0);
}

char* ms_setup_installing_json(void) {
    refresh_installing();
    return strdup(atomic_load(&g_installing) ? "{\"installing\":true}" : "{\"installing\":false}");
}

static char* setup_error(const char* message) {
    ms_json_writer w;
    char* out;
    ms_json_writer_init(&w);
    ms_json_writer_object_begin(&w);
    ms_json_writer_key(&w, "ok");
    ms_json_writer_bool(&w, false);
    ms_json_writer_key(&w, "error");
    ms_json_writer_string(&w, message);
    ms_json_writer_object_end(&w);
    out = ms_json_writer_take(&w);
    return out;
}

static bool run_brew_install(const char* package) {
    pid_t pid, waited;
    int wait_status;
    pid = fork();
    if (pid < 0)
        return false;
    if (pid == 0) {
        execlp("brew", "brew", "install", package, (char*)NULL);
        _exit(127);
    }
    do {
        waited = waitpid(pid, &wait_status, 0);
    } while (waited < 0 && errno == EINTR);
    return waited == pid && WIFEXITED(wait_status) && WEXITSTATUS(wait_status) == 0;
}

char* ms_setup_install_dependencies_json(const unsigned char* body, size_t body_length, int* status) {
    char error[128];
    ms_json* request;
    const ms_json* ids;
    ms_json_writer w;
    char* out;
    size_t i;
    if (status != NULL)
        *status = 200;
    request = ms_json_parse((const char*)(body == NULL ? (const unsigned char*)"{}" : body),
                            body == NULL ? 2 : body_length, error, sizeof(error));
    if (request == NULL || ms_json_type_of(request) != MS_JSON_OBJECT) {
        ms_json_free(request);
        if (status)
            *status = 400;
        return setup_error("invalid JSON body");
    }
    ids = ms_json_object_get(request, "ids");
    bool all_ok = true;
    ms_json_writer_init(&w);
    ms_json_writer_object_begin(&w);
    ms_json_writer_key(&w, "results");
    ms_json_writer_array_begin(&w);
    if (ids != NULL && ms_json_type_of(ids) == MS_JSON_ARRAY)
        for (i = 0; i < ms_json_array_length(ids); ++i) {
            char* id = NULL;
            const ms_json* value = ms_json_array_get(ids, i);
            bool known = false, installed = false;
            if (ms_json_as_string(value, &id))
                known = strcmp(id, "mono") == 0 || strcmp(id, "sdl3") == 0;
            if (known && command_available("brew"))
                installed = run_brew_install(id);
            if (!known || !installed)
                all_ok = false;
            ms_json_writer_object_begin(&w);
            ms_json_writer_key(&w, "id");
            ms_json_writer_string(&w, id == NULL ? "" : id);
            ms_json_writer_key(&w, "ok");
            ms_json_writer_bool(&w, known && installed);
            if (!known) {
                ms_json_writer_key(&w, "error");
                ms_json_writer_string(&w, "unknown dependency");
            } else if (!command_available("brew")) {
                ms_json_writer_key(&w, "error");
                ms_json_writer_string(&w, "brew command not found");
            } else if (!installed) {
                ms_json_writer_key(&w, "error");
                ms_json_writer_string(&w, "brew install failed");
            }
            ms_json_writer_object_end(&w);
            free(id);
        }
    ms_json_writer_array_end(&w);
    ms_json_writer_key(&w, "ok");
    ms_json_writer_bool(&w, all_ok);
    ms_json_writer_object_end(&w);
    out = ms_json_writer_take(&w);
    ms_json_free(request);
    return out;
}

static void close_install_worker_descriptors(void) {
    long limit = sysconf(_SC_OPEN_MAX);
    if (limit < 0 || limit > 16384)
        limit = 16384;
    for (int fd = 3; fd < limit; fd++)
        close(fd);
}

char* ms_setup_install_all_json(const char* metalsharp_home, int* status) {
    bool expected = false;
    pid_t pid;
    if (status != NULL)
        *status = 200;
    refresh_installing();
    if (!atomic_compare_exchange_strong(&g_installing, &expected, true))
        return strdup("{\"ok\":false,\"error\":\"installation already in progress\"}");
    pid = fork();
    if (pid < 0) {
        atomic_store(&g_installing, false);
        if (status)
            *status = 500;
        return setup_error("could not start installation worker");
    }
    if (pid == 0) {
        close_install_worker_descriptors();
        run_install_all_worker(metalsharp_home);
    }
    g_install_pid = pid;
    return strdup("{\"ok\":true}");
}

static bool vcpp_installer_downloaded(const char* path) {
    struct stat info;
    return path && stat(path, &info) == 0 && S_ISREG(info.st_mode) && info.st_size > 1000000;
}

static bool download_vcpp_installer(const char* home, bool x86, char** result) {
    const char* filename = x86 ? "vc_redist.x86.exe" : "vc_redist.x64.exe";
    const char* url =
        x86 ? "https://aka.ms/vs/17/release/vc_redist.x86.exe" : "https://aka.ms/vs/17/release/vc_redist.x64.exe";
    char* directory = join_path(home, "runtime/redist/vcredist");
    char* path = directory ? join_path(directory, filename) : NULL;
    char* temporary = path ? malloc(strlen(path) + 10) : NULL;
    pid_t pid;
    if (temporary)
        snprintf(temporary, strlen(path) + 10, "%s.download", path);
    if (!directory || !path || !temporary || !mkdir_p(directory))
        goto fail;
    if (vcpp_installer_downloaded(path)) {
        *result = path;
        free(directory);
        free(temporary);
        return true;
    }
    unlink(temporary);
    pid = fork();
    if (pid < 0)
        goto fail;
    if (pid == 0) {
        execl("/usr/bin/curl", "curl", "--fail", "--location", "--silent", "--show-error", "--retry", "3", "-o",
              temporary, url, (char*)NULL);
        _exit(127);
    }
    if (!child_succeeded(pid, &(int){0}) || !vcpp_installer_downloaded(temporary) || rename(temporary, path) != 0)
        goto fail;
    *result = path;
    free(directory);
    free(temporary);
    return true;
fail:
    if (temporary)
        unlink(temporary);
    free(directory);
    free(path);
    free(temporary);
    return false;
}

char* ms_setup_install_vcpp_json(const char* home, bool x86, int* status) {
    const char* const dlls_x64[] = {"vcruntime140.dll", "vcruntime140_1.dll", "msvcp140.dll"};
    const char* const dlls_x86[] = {"vcruntime140.dll", "msvcp140.dll"};
    const char* const* dlls = x86 ? dlls_x86 : dlls_x64;
    size_t dll_count = x86 ? 2 : 3;
    char *prefix = join_path(home, "prefix-steam"), *system32 = NULL, *syswow64 = NULL, *wine = NULL, *installer = NULL,
         *installer_dir = NULL, *out = NULL;
    int child_status = 0;
    pid_t pid, waited;
    bool verified = true;
    if (status)
        *status = 200;
    system32 = prefix ? join_path(prefix, "drive_c/windows/system32") : NULL;
    if (!system32 || access(system32, F_OK) != 0) {
        if (status)
            *status = 400;
        out = setup_error("Wine prefix not ready — install runtime and Steam first");
        goto done;
    }
    if (x86)
        syswow64 = prefix ? join_path(prefix, "drive_c/windows/syswow64") : NULL;
    /* Match Rust's vcpp_ensure_downloaded: ensure both cached redists before
     * launching either architecture's installer. */
    {
        char* unused = NULL;
        bool x64_ok = download_vcpp_installer(home, false, &unused);
        free(unused);
        unused = NULL;
        bool x86_ok = download_vcpp_installer(home, true, &unused);
        free(unused);
        if (!x64_ok || !x86_ok) {
            if (status)
                *status = 500;
            out = setup_error(x86 ? "VC++ x86 installer not found" : "VC++ x64 installer not found");
            goto done;
        }
    }
    installer = join_path(home, x86 ? "runtime/redist/vcredist/vc_redist.x86.exe"
                                    : "runtime/redist/vcredist/vc_redist.x64.exe");
    wine = join_path(home, "runtime/wine/bin/metalsharp-wine");
    if (!wine || access(wine, X_OK) != 0) {
        free(wine);
        wine = join_path(home, "runtime/wine/bin/wine");
    }
    if (!wine || access(wine, X_OK) != 0 || !installer || !vcpp_installer_downloaded(installer)) {
        if (status)
            *status = 500;
        out = setup_error(!wine || access(wine, X_OK) != 0
                              ? "MetalSharp Wine not found"
                              : (x86 ? "VC++ x86 installer not found" : "VC++ x64 installer not found"));
        goto done;
    }
    installer_dir = strdup(installer);
    if (installer_dir) {
        char* slash = strrchr(installer_dir, '/');
        if (slash)
            *slash = '\0';
    }
    pid = fork();
    if (pid < 0) {
        if (status)
            *status = 500;
        out = setup_error("could not start VC++ installer");
        goto done;
    }
    if (pid == 0) {
        char library_env[PATH_MAX * 2];
        char* args[] = {wine, "start", "/wait", "/unix", installer, "/install", NULL};
        setenv("WINEPREFIX", prefix, 1);
        setenv("WINEARCH", "win64", 1);
        setenv("WINEDEBUG", "-all", 1);
        snprintf(library_env, sizeof(library_env), "%s/runtime/wine/lib:%s/runtime/wine/lib/wine/x86_64-unix", home,
                 home);
#ifdef __APPLE__
        setenv("DYLD_FALLBACK_LIBRARY_PATH", library_env, 1);
#else
        setenv("LD_LIBRARY_PATH", library_env, 1);
#endif
        if (installer_dir)
            chdir(installer_dir);
        execv(wine, args);
        _exit(127);
    }
    do {
        waited = waitpid(pid, &child_status, 0);
    } while (waited < 0 && errno == EINTR);
    if (waited != pid || !WIFEXITED(child_status) ||
        (WEXITSTATUS(child_status) != 0 && WEXITSTATUS(child_status) != 194)) {
        if (status)
            *status = 500;
        out = setup_error(x86 ? "VC++ x86 installer failed" : "VC++ x64 installer failed");
        goto done;
    }
    for (size_t i = 0; i < dll_count; i++) {
        char* path = join_path(x86 ? syswow64 : system32, dlls[i]);
        struct stat info;
        if (!path || stat(path, &info) != 0 || info.st_size <= 10000)
            verified = false;
        free(path);
    }
    if (!verified) {
        if (status)
            *status = 500;
        out = setup_error(x86 ? "VC++ x86 installer completed, but runtime DLLs were not found in syswow64"
                              : "VC++ x64 installer completed, but runtime DLLs were not found in system32");
        goto done;
    }
    out = strdup("{\"ok\":true}");
done:
    free(prefix);
    free(system32);
    free(syswow64);
    free(wine);
    free(installer);
    free(installer_dir);
    return out;
}
