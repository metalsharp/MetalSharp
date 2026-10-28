#include "metalsharp_backend/json.h"
#include "metalsharp_backend/steam.h"

#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void expect_key(const char* home, const char* expected) {
    char* response = ms_steam_api_key_json(home);
    char error[128];
    char* key = NULL;
    assert(response != NULL);
    ms_json* json = ms_json_parse(response, strlen(response), error, sizeof(error));
    assert(json != NULL);
    assert(ms_json_as_string(ms_json_object_get(json, "key"), &key));
    assert(key != NULL && strcmp(key, expected) == 0);
    free(key);
    ms_json_free(json);
    free(response);
}

static void save_key(const char* home) {
    static const unsigned char body[] = "{\"key\":\"fixture-key\"}";
    int status = 0;
    char* response = ms_steam_save_api_key_json(home, body, sizeof(body) - 1, &status);
    assert(response != NULL && status == 200);
    free(response);
}

int main(int argc, char** argv) {
    char path[PATH_MAX], sentinel[PATH_MAX];
    struct stat info;
    const char* home = argc > 1 ? argv[1] : NULL;
    assert(home != NULL);
    assert(setenv("HOME", home, 1) == 0);
    assert(setenv("METALSHARP_HOME", home, 1) == 0);
    snprintf(path, sizeof(path), "%s/cache/steam_config.json", home);
    snprintf(sentinel, sizeof(sentinel), "%s/sentinel.json", home);

    mode_t previous = umask(0);
    save_key(home);
    assert(stat(path, &info) == 0 && (info.st_mode & 0777) == 0600);
    expect_key(home, "fixture-key");

    assert(chmod(path, 0644) == 0);
    expect_key(home, "fixture-key");
    assert(stat(path, &info) == 0 && (info.st_mode & 0777) == 0600);

    assert(chmod(path, 0644) == 0);
    save_key(home);
    assert(stat(path, &info) == 0 && (info.st_mode & 0777) == 0600);

    FILE* file = fopen(sentinel, "wb");
    static const char sentinel_text[] = "{\"steam_api_key\":\"sentinel-key\"}";
    assert(file != NULL && fputs(sentinel_text, file) >= 0);
    assert(fclose(file) == 0 && chmod(sentinel, 0644) == 0);
    assert(unlink(path) == 0 && symlink(sentinel, path) == 0);
    expect_key(home, "");
    assert(stat(sentinel, &info) == 0 && (info.st_mode & 0777) == 0644);

    save_key(home);
    assert(lstat(path, &info) == 0 && S_ISREG(info.st_mode) && (info.st_mode & 0777) == 0600);
    expect_key(home, "fixture-key");
    char content[128] = "";
    file = fopen(sentinel, "rb");
    assert(file != NULL);
    assert(fread(content, 1, sizeof(content) - 1, file) == sizeof(sentinel_text) - 1);
    assert(fclose(file) == 0 && strcmp(content, sentinel_text) == 0);
    assert(stat(sentinel, &info) == 0 && (info.st_mode & 0777) == 0644);

    umask(previous);
    puts("Steam credential permissions tests passed");
    return 0;
}
