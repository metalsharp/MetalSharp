#include "metalsharp_backend/updater.h"

#include "metalsharp_backend/backend.h"
#include "metalsharp_backend/json.h"
#include "metalsharp_backend/json_writer.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define MS_RELEASE_API "https://api.github.com/repos/aaf2tbz/metalsharp/releases/latest"

static char* join_path(const char* left, const char* right) {
    size_t a = strlen(left), b = strlen(right);
    bool slash = a > 0 && left[a - 1] != '/';
    char* path = (char*)malloc(a + b + (slash ? 2 : 1));
    if (path != NULL)
        (void)snprintf(path, a + b + (slash ? 2 : 1), "%s%s%s", left, slash ? "/" : "", right);
    return path;
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

static char* read_file(const char* path, size_t max_size) {
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
    if (size < 0 || (size_t)size > max_size || fseek(file, 0, SEEK_SET) != 0) {
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
    return data;
}

static char* curl_release_json(void) {
    const char* fixture = getenv("METALSHARP_UPDATE_RELEASE_JSON");
    if (fixture != NULL && fixture[0] != '\0')
        return read_file(fixture, 8 * 1024 * 1024);
    FILE* pipe =
        popen("/usr/bin/curl --fail --silent --show-error --location --max-time 20 -A 'MetalSharp/" MS_BACKEND_VERSION
              "' '" MS_RELEASE_API "'",
              "r");
    char* data;
    size_t length = 0, capacity = 16384;
    if (pipe == NULL)
        return NULL;
    data = (char*)malloc(capacity);
    if (data == NULL) {
        pclose(pipe);
        return NULL;
    }
    while (!feof(pipe)) {
        size_t n;
        if (length + 4096 + 1 > capacity) {
            capacity *= 2;
            if (capacity > 8 * 1024 * 1024) {
                free(data);
                pclose(pipe);
                return NULL;
            }
            data = (char*)realloc(data, capacity);
            if (data == NULL) {
                pclose(pipe);
                return NULL;
            }
        }
        n = fread(data + length, 1, 4096, pipe);
        length += n;
        if (ferror(pipe)) {
            free(data);
            pclose(pipe);
            return NULL;
        }
    }
    data[length] = '\0';
    if (pclose(pipe) != 0) {
        free(data);
        return NULL;
    }
    return data;
}

static char* clean_version(const char* tag) {
    char* result = (char*)calloc(1, 64);
    const char* cursor = tag == NULL ? "" : tag;
    size_t out = 0;
    bool first = true;
    if (result == NULL)
        return NULL;
    while (*cursor == ' ' || *cursor == '\t' || *cursor == '\r' || *cursor == '\n')
        cursor++;
    if (*cursor == 'v' || *cursor == 'V')
        cursor++;
    while (*cursor != '\0' && out < 63) {
        size_t digits = 0;
        while (isdigit((unsigned char)cursor[digits]))
            digits++;
        if (digits == 0) {
            while (*cursor != '\0' && *cursor != '.')
                cursor++;
            if (*cursor == '.')
                cursor++;
            continue;
        }
        if (!first)
            result[out++] = '.';
        if (out + digits >= 64)
            break;
        memcpy(result + out, cursor, digits);
        out += digits;
        first = false;
        cursor += digits;
        if (*cursor == '.')
            cursor++;
        else
            while (*cursor != '\0' && *cursor != '.')
                cursor++;
    }
    result[out] = '\0';
    return result;
}

static int version_compare(const char* left, const char* right) {
    const char *a = left, *b = right;
    while (*a != '\0' || *b != '\0') {
        unsigned long av = 0, bv = 0;
        while (isdigit((unsigned char)*a)) {
            av = av * 10 + (unsigned)(*a - '0');
            a++;
        }
        while (isdigit((unsigned char)*b)) {
            bv = bv * 10 + (unsigned)(*b - '0');
            b++;
        }
        if (av < bv)
            return -1;
        if (av > bv)
            return 1;
        if (*a == '.')
            a++;
        else if (*a != '\0')
            while (*a != '\0' && *a != '.')
                a++;
        if (*b == '.')
            b++;
        else if (*b != '\0')
            while (*b != '\0' && *b != '.')
                b++;
    }
    return 0;
}

static char* release_field_json(const ms_json* release, const char* field) {
    char* value = NULL;
    (void)ms_json_as_string(ms_json_object_get(release, field), &value);
    return value;
}

static bool contains_case_insensitive(const char* text, const char* needle) {
    size_t needle_length;
    if (text == NULL || needle == NULL || (needle_length = strlen(needle)) == 0)
        return false;
    for (; *text != '\0'; ++text) {
        size_t i = 0;
        while (i < needle_length && text[i] != '\0' &&
               tolower((unsigned char)text[i]) == tolower((unsigned char)needle[i]))
            i++;
        if (i == needle_length)
            return true;
    }
    return false;
}

static bool has_dmg_suffix(const char* name) {
    size_t length = name == NULL ? 0 : strlen(name);
    return length >= 4 && name[length - 4] == '.' && tolower((unsigned char)name[length - 3]) == 'd' &&
           tolower((unsigned char)name[length - 2]) == 'm' && tolower((unsigned char)name[length - 1]) == 'g';
}

static int host_macos_major(void) {
    const char* override = getenv("METALSHARP_UPDATE_HOST_MACOS");
    char version[64] = "";
    if (override != NULL && override[0] != '\0')
        return atoi(override);
    FILE* pipe = popen("/usr/bin/sw_vers -productVersion", "r");
    if (pipe == NULL)
        return 0;
    if (fgets(version, sizeof(version), pipe) == NULL)
        version[0] = '\0';
    if (pclose(pipe) != 0)
        return 0;
    return atoi(version);
}

typedef struct {
    char* regular_url;
    unsigned long long regular_size;
    bool regular_arm64;
    char* fex_url;
    unsigned long long fex_size;
    bool fex_arm64;
} release_assets;

static void release_assets_free(release_assets* selected) {
    free(selected->regular_url);
    free(selected->fex_url);
    memset(selected, 0, sizeof(*selected));
}

static void release_assets_find(const ms_json* release, release_assets* selected) {
    const ms_json* assets = ms_json_object_get(release, "assets");
    size_t i;
    memset(selected, 0, sizeof(*selected));
    for (i = 0; i < ms_json_array_length(assets); ++i) {
        const ms_json* asset = ms_json_array_get(assets, i);
        char* name = release_field_json(asset, "name");
        if (!has_dmg_suffix(name)) {
            free(name);
            continue;
        }
        char* candidate_url = release_field_json(asset, "browser_download_url");
        long long candidate_size = 0;
        (void)ms_json_as_i64(ms_json_object_get(asset, "size"), &candidate_size);
        bool fex = contains_case_insensitive(name, "FEX");
        bool arm64 = contains_case_insensitive(name, "arm64");
        free(name);
        char** selected_url = fex ? &selected->fex_url : &selected->regular_url;
        unsigned long long* selected_size = fex ? &selected->fex_size : &selected->regular_size;
        bool* selected_arm64 = fex ? &selected->fex_arm64 : &selected->regular_arm64;
        if (candidate_url == NULL || candidate_url[0] == '\0' ||
            (*selected_url != NULL && (!arm64 || *selected_arm64))) {
            free(candidate_url);
            continue;
        }
        free(*selected_url);
        *selected_url = candidate_url;
        *selected_size = candidate_size > 0 ? (unsigned long long)candidate_size : 0;
        *selected_arm64 = arm64;
    }
}

char* ms_update_check_json(void) {
    char* text = curl_release_json();
    char error[128];
    ms_json* release;
    char *tag, *latest, *name, *body;
    release_assets assets;
    int macos_major;
    ms_json_writer writer;
    char* result;
    if (text == NULL)
        return strdup("{\"ok\":false,\"error\":\"failed to fetch release\",\"current_version\":\"0.61.0\"}");
    release = ms_json_parse(text, strlen(text), error, sizeof(error));
    free(text);
    if (release == NULL)
        return strdup("{\"ok\":false,\"error\":\"failed to parse release\",\"current_version\":\"0.61.0\"}");
    tag = release_field_json(release, "tag_name");
    latest = clean_version(tag);
    name = release_field_json(release, "name");
    body = release_field_json(release, "body");
    release_assets_find(release, &assets);
    macos_major = host_macos_major();
    ms_json_writer_init(&writer);
    ms_json_writer_object_begin(&writer);
    ms_json_writer_key(&writer, "ok");
    ms_json_writer_bool(&writer, true);
    ms_json_writer_key(&writer, "current_version");
    ms_json_writer_string(&writer, MS_BACKEND_VERSION);
    ms_json_writer_key(&writer, "latest_version");
    ms_json_writer_string(&writer, latest == NULL ? "" : latest);
    ms_json_writer_key(&writer, "available");
    ms_json_writer_bool(&writer, latest != NULL && version_compare(latest, MS_BACKEND_VERSION) > 0);
    ms_json_writer_key(&writer, "download_url");
    ms_json_writer_string(&writer, assets.regular_url == NULL ? "" : assets.regular_url);
    ms_json_writer_key(&writer, "download_size");
    ms_json_writer_u64(&writer, assets.regular_size);
    ms_json_writer_key(&writer, "fex_available");
    ms_json_writer_bool(&writer, assets.fex_url != NULL);
    ms_json_writer_key(&writer, "fex_download_url");
    ms_json_writer_string(&writer, assets.fex_url == NULL ? "" : assets.fex_url);
    ms_json_writer_key(&writer, "fex_download_size");
    ms_json_writer_u64(&writer, assets.fex_size);
    ms_json_writer_key(&writer, "fex_supported");
    ms_json_writer_bool(&writer, macos_major >= 27);
    ms_json_writer_key(&writer, "macos_major");
    ms_json_writer_i64(&writer, macos_major);
    ms_json_writer_key(&writer, "release_notes");
    ms_json_writer_string(&writer, body == NULL ? "" : body);
    ms_json_writer_key(&writer, "release_name");
    ms_json_writer_string(&writer, name == NULL ? (tag == NULL ? "" : tag) : name);
    ms_json_writer_object_end(&writer);
    result = ms_json_writer_take(&writer);
    free(tag);
    free(latest);
    free(name);
    free(body);
    release_assets_free(&assets);
    ms_json_free(release);
    return result;
}

static char* progress_path(const char* home) {
    return join_path(home, "update_progress.json");
}

static bool write_progress(const char* home, const char* status, unsigned percent, const char* message,
                           const char* error) {
    char* path = progress_path(home);
    ms_json_writer writer;
    char* json;
    FILE* file;
    bool ok;
    if (path == NULL)
        return false;
    ms_json_writer_init(&writer);
    ms_json_writer_object_begin(&writer);
    ms_json_writer_key(&writer, "status");
    ms_json_writer_string(&writer, status);
    ms_json_writer_key(&writer, "percent");
    ms_json_writer_u64(&writer, percent);
    ms_json_writer_key(&writer, "message");
    ms_json_writer_string(&writer, message);
    ms_json_writer_key(&writer, "error");
    if (error == NULL)
        ms_json_writer_null(&writer);
    else
        ms_json_writer_string(&writer, error);
    ms_json_writer_object_end(&writer);
    json = ms_json_writer_take(&writer);
    if (json == NULL) {
        free(path);
        return false;
    }
    file = fopen(path, "wb");
    ok = file != NULL && fputs(json, file) >= 0 && fclose(file) == 0;
    if (file != NULL && !ok)
        fclose(file);
    free(json);
    free(path);
    return ok;
}

char* ms_update_progress_json(const char* metalsharp_home) {
    char* path = progress_path(metalsharp_home);
    char* text;
    if (path == NULL)
        return NULL;
    text = read_file(path, 1024 * 1024);
    free(path);
    return text == NULL ? strdup("{\"status\":\"idle\",\"percent\":0,\"message\":\"\",\"error\":null}") : text;
}

char* ms_update_cleanup_json(const char* metalsharp_home) {
    char* dir = join_path(metalsharp_home, "cache/updates");
    DIR* entries;
    struct dirent* entry;
    unsigned long long removed = 0, bytes = 0;
    ms_json_writer writer;
    char* result;
    if (dir != NULL && (entries = opendir(dir)) != NULL) {
        while ((entry = readdir(entries)) != NULL) {
            size_t length = strlen(entry->d_name);
            if (length < 4 || strcmp(entry->d_name + length - 4, ".dmg") != 0)
                continue;
            char* path = join_path(dir, entry->d_name);
            struct stat st;
            if (path != NULL && stat(path, &st) == 0 && unlink(path) == 0) {
                removed++;
                if (st.st_size > 0)
                    bytes += (unsigned long long)st.st_size;
            }
            free(path);
        }
        closedir(entries);
    }
    ms_json_writer_init(&writer);
    ms_json_writer_object_begin(&writer);
    ms_json_writer_key(&writer, "ok");
    ms_json_writer_bool(&writer, true);
    ms_json_writer_key(&writer, "removed");
    ms_json_writer_u64(&writer, removed);
    ms_json_writer_key(&writer, "bytes_freed");
    ms_json_writer_u64(&writer, bytes);
    ms_json_writer_object_end(&writer);
    result = ms_json_writer_take(&writer);
    free(dir);
    return result;
}

char* ms_update_dmg_path_json(const char* metalsharp_home, const char* variant) {
    char* dir = join_path(metalsharp_home, "cache/updates");
    DIR* entries;
    struct dirent* entry;
    char* found = NULL;
    char* version = NULL;
    bool want_fex = variant != NULL && strcmp(variant, "fex") == 0;
    if (dir != NULL && (entries = opendir(dir)) != NULL) {
        while ((entry = readdir(entries)) != NULL) {
            size_t length = strlen(entry->d_name);
            bool is_fex = contains_case_insensitive(entry->d_name, "FEX");
            if (length > 15 && strncmp(entry->d_name, "MetalSharp-", 11) == 0 && has_dmg_suffix(entry->d_name) &&
                is_fex == want_fex) {
                free(found);
                found = join_path(dir, entry->d_name);
                free(version);
                size_t version_length = length - 15;
                if (is_fex && version_length > 4 &&
                    contains_case_insensitive(entry->d_name + 11 + version_length - 4, "-FEX"))
                    version_length -= 4;
                version = strndup(entry->d_name + 11, version_length);
            }
        }
        closedir(entries);
    }
    free(dir);
    if (found == NULL)
        return strdup("{\"ok\":false,\"error\":\"no downloaded DMG\"}");
    {
        ms_json_writer writer;
        char* result;
        ms_json_writer_init(&writer);
        ms_json_writer_object_begin(&writer);
        ms_json_writer_key(&writer, "ok");
        ms_json_writer_bool(&writer, true);
        ms_json_writer_key(&writer, "path");
        ms_json_writer_string(&writer, found);
        ms_json_writer_key(&writer, "version");
        ms_json_writer_string(&writer, version == NULL ? "" : version);
        ms_json_writer_object_end(&writer);
        result = ms_json_writer_take(&writer);
        free(found);
        free(version);
        return result;
    }
}

char* ms_update_start_json(const char* metalsharp_home, const unsigned char* body, size_t body_length) {
    bool fex = false;
    if (body_length > 0) {
        char parse_error[128];
        ms_json* request = ms_json_parse((const char*)body, body_length, parse_error, sizeof(parse_error));
        char* variant = request == NULL ? NULL : release_field_json(request, "variant");
        if (request == NULL || variant == NULL || (strcmp(variant, "regular") != 0 && strcmp(variant, "fex") != 0)) {
            free(variant);
            ms_json_free(request);
            return strdup("{\"ok\":false,\"error\":\"update variant must be regular or fex\"}");
        }
        fex = strcmp(variant, "fex") == 0;
        free(variant);
        ms_json_free(request);
    }
    if (fex && host_macos_major() < 27)
        return strdup("{\"ok\":false,\"error\":\"FEX updates require macOS 27 or newer\"}");
    char* lock = join_path(metalsharp_home, "update.lock");
    char* dir = join_path(metalsharp_home, "cache/updates");
    if (lock == NULL || dir == NULL || mkdir_p(dir) == false) {
        free(lock);
        free(dir);
        return strdup("{\"ok\":false,\"error\":\"failed to start update\"}");
    }
    {
        int fd = open(lock, O_CREAT | O_EXCL | O_WRONLY, 0600);
        if (fd < 0 && errno == EEXIST) {
            char* owner_text = read_file(lock, 64);
            long owner = owner_text ? strtol(owner_text, NULL, 10) : 0;
            free(owner_text);
            if (owner <= 0 || (kill((pid_t)owner, 0) != 0 && errno == ESRCH)) {
                unlink(lock);
                fd = open(lock, O_CREAT | O_EXCL | O_WRONLY, 0600);
            }
        }
        if (fd < 0) {
            free(lock);
            free(dir);
            return strdup("{\"ok\":false,\"error\":\"update already in progress\"}");
        }
        dprintf(fd, "%ld\n", (long)getpid());
        close(fd);
    }
    (void)write_progress(metalsharp_home, "starting", 0, "Checking for updates...", NULL);
    pid_t child = fork();
    if (child == 0) {
        int owner_fd = open(lock, O_WRONLY | O_TRUNC);
        if (owner_fd >= 0) {
            dprintf(owner_fd, "%ld\n", (long)getpid());
            close(owner_fd);
        }
        (void)write_progress(metalsharp_home, "checking", 5, "Fetching latest release info...", NULL);
        char* info = ms_update_check_json();
        char error[128];
        ms_json* json = info == NULL ? NULL : ms_json_parse(info, strlen(info), error, sizeof(error));
        char* url = json == NULL ? NULL : release_field_json(json, fex ? "fex_download_url" : "download_url");
        char* latest = json == NULL ? NULL : release_field_json(json, "latest_version");
        free(info);
        ms_json_free(json);
        if (url == NULL || latest == NULL || url[0] == '\0') {
            (void)write_progress(metalsharp_home, "error", 0, "No DMG download URL found", "no_download_url");
        } else {
            size_t name_size = strlen(latest) + (fex ? 20 : 16);
            char* name = (char*)malloc(name_size);
            char* tmp;
            if (name != NULL) {
                (void)snprintf(name, name_size, fex ? "MetalSharp-%s-FEX.dmg" : "MetalSharp-%s.dmg", latest);
                tmp = join_path(dir, "download.dmg.tmp");
                char* dest = join_path(dir, name);
                bool downloaded = false;
                (void)write_progress(metalsharp_home, "downloading", 10, "Downloading DMG...", NULL);
                if (tmp != NULL && dest != NULL) {
                    pid_t curl_pid = fork();
                    if (curl_pid == 0) {
                        execl("/usr/bin/curl", "curl", "--fail", "--location", "--silent", "--show-error", "-A",
                              "MetalSharp/" MS_BACKEND_VERSION, "-o", tmp, url, (char*)NULL);
                        _exit(127);
                    }
                    if (curl_pid > 0) {
                        int curl_status = 0;
                        if (waitpid(curl_pid, &curl_status, 0) == curl_pid && WIFEXITED(curl_status) &&
                            WEXITSTATUS(curl_status) == 0)
                            downloaded = true;
                    }
                }
                if (downloaded && rename(tmp, dest) == 0) {
                    (void)write_progress(metalsharp_home, "downloaded", 80, "Download complete — ready to install",
                                         NULL);
                } else {
                    (void)write_progress(metalsharp_home, "error", 0, "Download failed", "download_failed");
                    if (tmp != NULL)
                        unlink(tmp);
                }
                free(name);
                free(tmp);
                free(dest);
            }
        }
        unlink(lock);
        _exit(0);
    }
    free(lock);
    free(dir);
    if (child < 0) {
        unlink(lock);
        free(lock);
        free(dir);
        return strdup("{\"ok\":false,\"error\":\"failed to spawn updater\"}");
    }
    return strdup("{\"ok\":true}");
}
