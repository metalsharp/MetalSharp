// Exercise GOG bootstrap trust boundaries without networking or running a downloaded helper.
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE
#endif
#include <unistd.h>
static int fixture_execv(const char* path, char* const argv[]);
static int fixture_execlp(const char* file, const char* arg, ...);
#define execv  fixture_execv
#define execlp fixture_execlp
#include "../runtime/gog.c"
#undef execv
#undef execlp
#include <assert.h>

static const char* download_fixture;
static bool download_failure;

static int fixture_execv(const char* path, char* const argv[]) {
    assert(!strcmp(path, "/usr/bin/curl"));
    const char* output = NULL;
    bool https = false, redirects = false, pinned = false;
    for (size_t i = 1; argv[i]; i++) {
        if (!strcmp(argv[i], "-o"))
            output = argv[i + 1];
        if (!strcmp(argv[i], "--proto"))
            https = argv[i + 1] && !strcmp(argv[i + 1], "=https");
        if (!strcmp(argv[i], "--proto-redir"))
            redirects = argv[i + 1] && !strcmp(argv[i + 1], "=https");
        if (!strcmp(argv[i], GOGDL_URL))
            pinned = true;
    }
    assert(output && https && redirects && pinned);
    assert(!strstr(GOGDL_URL, "/latest/"));
    struct stat st;
    assert(stat(output, &st) == 0 && (st.st_mode & 0777) == 0600);
    if (download_failure) {
        errno = EIO;
        return -1;
    }
    FILE* out = fopen(output, "wb");
    assert(out);
    if (download_fixture) {
        FILE* input = fopen(download_fixture, "rb");
        unsigned char buffer[8192];
        size_t got;
        assert(input);
        while ((got = fread(buffer, 1, sizeof(buffer), input)) > 0)
            assert(fwrite(buffer, 1, got, out) == got);
        assert(!ferror(input) && fclose(input) == 0);
    } else {
        assert(fputs("tampered helper", out) >= 0);
    }
    assert(fclose(out) == 0);
    _exit(0);
}

static int fixture_execlp(const char* file, const char* arg, ...) {
    assert(!strcmp(file, "curl") && !strcmp(arg, "curl"));
    static const char response[] = "{\"owned\":[]}";
    assert(write(STDOUT_FILENO, response, sizeof(response) - 1) == sizeof(response) - 1);
    _exit(0);
}

static void write_fixture(const char* path, const char* text, mode_t mode) {
    FILE* file = fopen(path, "wb");
    assert(file && fputs(text, file) >= 0 && fclose(file) == 0);
    assert(chmod(path, mode) == 0);
}

static void assert_no_temporary_downloads(const char* tools) {
    DIR* dir = opendir(tools);
    assert(dir);
    struct dirent* entry;
    while ((entry = readdir(dir)) != NULL)
        assert(strncmp(entry->d_name, ".gogdl-download-", 16));
    closedir(dir);
}

int main(int argc, char** argv) {
    char template[] = "/tmp/metalsharp-gog-bootstrap-'XXXXXX";
    char* home = mkdtemp(template);
    char error[256];
    assert(home);
    char* tools = join(home, "tools");
    char* managed = join(tools, "gogdl");
    char* alias_dir = join(home, "aliases");
    char* alias = join(alias_dir, "gogdl");
    char* digest_fixture = join(home, "digest");
    assert(tools && managed && alias_dir && alias && digest_fixture);
    assert(mkdir_p(tools) && mkdir_p(alias_dir));
    write_fixture(digest_fixture, "abc", 0600);
    assert(gogdl_sha256_matches(digest_fixture, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    assert(!gogdl_sha256_matches(digest_fixture, GOGDL_SHA256));
    assert(unlink(digest_fixture) == 0 && !gogdl_sha256_matches(digest_fixture, GOGDL_SHA256));

    const char* legacy = "#!/bin/sh\nexit 99\n";
    write_fixture(managed, legacy, 0755);
    assert(symlink(managed, alias) == 0);
    assert(setenv("PATH", alias_dir, 1) == 0 && unsetenv("METALSHARP_GOGDL_BIN") == 0);
    assert(!gogdl_available(home));
    assert(!ensure_gogdl_available(home, error, sizeof(error)));
    assert(strstr(error, "integrity validation"));
    char* old = read_text(managed);
    assert(old && !strcmp(old, legacy));
    free(old);
    assert_no_temporary_downloads(tools);

    download_failure = true;
    assert(!ensure_gogdl_available(home, error, sizeof(error)));
    assert(strstr(error, "download failed"));
    assert_no_temporary_downloads(tools);
    download_failure = false;

    assert(setenv("METALSHARP_GOGDL_BIN", managed, 1) == 0);
    char* selected = gogdl_path(home);
    assert(selected && !strcmp(selected, managed));
    free(selected);
    assert(ensure_gogdl_available(home, error, sizeof(error)));
    assert(unsetenv("METALSHARP_GOGDL_BIN") == 0);

    char* external = join(home, "external-gogdl");
    write_fixture(external, "#!/bin/sh\nexit 0\n", 0755);
    assert(unlink(alias) == 0 && symlink(external, alias) == 0);
    selected = gogdl_path(home);
    assert(selected && !strcmp(selected, alias));
    free(selected);
    assert(ensure_gogdl_available(home, error, sizeof(error)));
    assert(unlink(alias) == 0);

    char* prefix = join(home, "bottles/gog-prefix/prefix/drive_c");
    char* save = join(prefix, "save.dat");
    char* ready = join(home, "bottles/gog-prefix/prefix/.metalsharp-prefix-ready");
    char* auth = auth_path(home);
    char* auth_dir = join(home, "gog_store");
    assert(prefix && save && ready && auth && auth_dir && mkdir_p(prefix) && mkdir_p(auth_dir));
    static const char credential[] = "{\"access_token\":\"keep-token\"}";
    write_fixture(auth, credential, 0600);
    write_fixture(save, "keep-save", 0600);
    write_fixture(ready, "2\n", 0600);
    char* status = ms_gog_status_json(home);
    assert(status && strstr(status, "\"prefixInitialized\":true") && strstr(status, "\"gogdlAvailable\":false"));
    free(status);
    char* response = ms_gog_action_json(home, "sync", NULL, 0);
    assert(response && strstr(response, "integrity validation"));
    free(response);
    assert_no_temporary_downloads(tools);

    if (argc > 1) {
        download_fixture = argv[1];
        assert(gogdl_sha256_matches(download_fixture, GOGDL_SHA256));
        response = ms_gog_action_json(home, "sync", NULL, 0);
        assert(response && strstr(response, "\"ok\":true"));
        free(response);
        selected = gogdl_path(home);
        assert(selected && !strcmp(selected, managed));
        free(selected);
        struct stat st;
        assert(stat(managed, &st) == 0 && (st.st_mode & 0777) == 0755);
        assert_no_temporary_downloads(tools);
    }
    char* stored = read_text(auth);
    assert(stored && !strcmp(stored, credential));
    free(stored);
    stored = read_text(save);
    assert(stored && !strcmp(stored, "keep-save"));
    free(stored);
    status = ms_gog_status_json(home);
    assert(status && strstr(status, "\"prefixInitialized\":true"));
    free(status);
    free(auth_dir);
    free(auth);
    free(ready);
    free(save);
    free(prefix);
    assert(remove_tree(home));
    free(external);
    free(digest_fixture);
    free(alias);
    free(alias_dir);
    free(managed);
    free(tools);
    puts("GOG bootstrap integrity tests passed");
    return 0;
}
